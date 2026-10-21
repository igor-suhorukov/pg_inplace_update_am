# Many concurrent writers and readers (pgbench): money is neither created
# nor destroyed, and the result matches what the invariants require.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IheapTest;

my $node = iheap_node('concurrency',
	extra_conf => "max_connections = 50\ndeadlock_timeout = 100ms");

$node->safe_psql('postgres', q{
CREATE TABLE acct (id int PRIMARY KEY, branch int, balance int, note text) USING iheap;
CREATE INDEX acct_branch ON acct (branch);
INSERT INTO acct SELECT g, g % 10, 1000, 'n' FROM generate_series(1, 200) g;
CREATE TABLE hist (id bigserial, a int, b int, amount int) USING iheap;
-- a reader's snapshot must always see a consistent total
CREATE FUNCTION check_total() RETURNS void LANGUAGE plpgsql AS $$
DECLARE s bigint; c bigint;
BEGIN
  SELECT sum(balance), count(*) INTO s, c FROM acct;
  IF s <> 200000 OR c <> 200 THEN
    RAISE EXCEPTION 'inconsistent snapshot: sum %, count %', s, c;
  END IF;
END $$;
});

# transfers (in place), branch moves (indexed column: rows move), note
# growth/shrink, aborted transfers, and readers checking the invariant
$node->pgbench(
	'--no-vacuum --client=8 --jobs=4 --transactions=400 --max-tries=100',
	0,
	[qr{processed: 3200/3200}],
	[qr{^$}],
	'concurrent transfers',
	{
		'001_transfer' => q{
\set a random(1, 200)
\set b random(1, 200)
\set amt random(1, 50)
BEGIN;
UPDATE acct SET balance = balance - :amt WHERE id = :a;
UPDATE acct SET balance = balance + :amt WHERE id = :b;
INSERT INTO hist (a, b, amount) VALUES (:a, :b, :amt);
COMMIT;
},
		'002_move' => q{
\set a random(1, 200)
\set br random(0, 9)
UPDATE acct SET branch = :br, note = repeat('m', :br * 10) WHERE id = :a;
},
		'003_abort' => q{
\set a random(1, 200)
BEGIN;
UPDATE acct SET balance = balance + 1000000 WHERE id = :a;
UPDATE acct SET branch = branch + 100 WHERE id = :a;
ROLLBACK;
},
		'004_check' => q{
SELECT check_total();
},
		'010_verify' => q{
SELECT iheap_verify_strict('acct');
}
	});

is($node->safe_psql('postgres', 'SELECT sum(balance), count(*) FROM acct'),
	'200000|200', 'balance preserved');
is($node->safe_psql('postgres', q{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM acct WHERE branch BETWEEN 0 AND 9}), '200',
	'branch index finds every row');
is($node->safe_psql('postgres', 'SELECT count(*) FROM acct WHERE branch >= 100'),
	'0', 'aborted moves are invisible');

# the same under REPEATABLE READ with retries done by pgbench
$node->pgbench(
	'--no-vacuum --client=8 --jobs=4 --transactions=300 --max-tries=100',
	0,
	[qr{processed: 2400/2400}],
	[qr{^$}],
	'concurrent transfers, repeatable read',
	{
		'005_transfer_rr' => q{
\set a random(1, 200)
\set b random(1, 200)
\set amt random(1, 50)
BEGIN ISOLATION LEVEL REPEATABLE READ;
UPDATE acct SET balance = balance - :amt WHERE id = :a;
UPDATE acct SET balance = balance + :amt WHERE id = :b;
COMMIT;
}
	});
is($node->safe_psql('postgres', 'SELECT sum(balance), count(*) FROM acct'),
	'200000|200', 'balance preserved under repeatable read');

# VACUUM concurrently with the workload
$node->pgbench(
	'--no-vacuum --client=4 --jobs=2 --transactions=200 --max-tries=100',
	0,
	[qr{processed: 800/800}],
	[qr{^$}],
	'transfers while vacuuming',
	{
		'006_transfer' => q{
\set a random(1, 200)
\set b random(1, 200)
BEGIN;
UPDATE acct SET balance = balance - 1 WHERE id = :a;
UPDATE acct SET balance = balance + 1 WHERE id = :b;
COMMIT;
},
		'007_vacuum' => q{
VACUUM acct;
},
		'011_verify' => q{
SELECT iheap_verify_strict('acct');
}
	});

is($node->safe_psql('postgres', 'SELECT sum(balance), count(*) FROM acct'),
	'200000|200', 'balance preserved with concurrent vacuum');

# space stays bounded: 200 short rows, after about a thousand row moves
# (rows spread over pages as moves place them; VACUUM makes the old slots
# reusable)
$node->safe_psql('postgres', 'VACUUM acct');
my $pages = $node->safe_psql('postgres', 'SELECT data_pages FROM iheap_relation_stats(\'acct\')');
cmp_ok($pages, '<=', 20, 'data pages stay bounded');
is($node->safe_psql('postgres', 'SELECT live_undo_pages FROM iheap_metapage_info(\'acct\')'),
	'0', 'undo discarded after the workload');

# parallel VACUUM (§21 D44) concurrently with key changes in place, whose
# marked entries of acct_branch the workers clean up, and with rows that
# grow and move: the checks run alongside
$node->pgbench(
	'--no-vacuum --client=6 --jobs=3 --transactions=150 --max-tries=100',
	0,
	[qr{processed: 900/900}],
	[qr{^$}],
	'key changes while vacuuming in parallel',
	{
		'013_branch@3' => q{
\set a random(1, 200)
\set br random(0, 9)
UPDATE acct SET branch = :br WHERE id = :a;
},
		'014_grow@3' => q{
\set a random(1, 200)
\set n random(0, 90)
UPDATE acct SET note = repeat('g', :n) WHERE id = :a;
},
		'015_vacuum_parallel@1' => q{
SET min_parallel_index_scan_size = 0;
VACUUM (PARALLEL 2) acct;
},
		'016_verify@1' => q{
SELECT iheap_verify_strict('acct');
}
	});
is($node->safe_psql('postgres', 'SELECT sum(balance), count(*) FROM acct'),
	'200000|200', 'balance preserved with concurrent parallel vacuum');

# the worker is launched (two indexes: one worker beside the leader)
my ($stdout, $stderr);
$node->psql('postgres', q{
UPDATE acct SET branch = (branch + 1) % 10;
SET min_parallel_index_scan_size = 0;
VACUUM (PARALLEL 2, VERBOSE) acct;
}, stdout => \$stdout, stderr => \$stderr);
like($stderr,
	qr/launched 1 parallel vacuum worker for index (?:vacuuming|cleanup)/,
	'parallel vacuum launches a worker');
is($node->safe_psql('postgres', 'SELECT marked FROM dmbtree_index_stats(\'acct_branch\')'),
	'0', 'marked entries cleaned up by a parallel vacuum');
is(iheap_verify($node, 'acct'), '', 'structure checks after the parallel vacuums');

# dmbtree: concurrent renames on a unique column, in place.  Collisions
# are caught (subtransaction rollbacks leave marked entries behind); a
# rename to a key another transaction is giving up or taking waits for it.
$node->safe_psql('postgres', q{
CREATE TABLE codes (id int PRIMARY KEY, code int) USING iheap;
CREATE UNIQUE INDEX codes_code ON codes USING dmbtree (code);
INSERT INTO codes SELECT g, g FROM generate_series(1, 100) g;
CREATE FUNCTION rename(a int, c int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  UPDATE codes SET code = c WHERE id = a;
EXCEPTION WHEN unique_violation THEN
  NULL;
END $$;
});
$node->pgbench(
	'--no-vacuum --client=8 --jobs=4 --transactions=500 --max-tries=100',
	0,
	[qr{processed: 4000/4000}],
	[qr{^$}],
	'concurrent unique renames',
	{
		'008_rename' => q{
\set a random(1, 100)
\set c random(1, 150)
SELECT rename(:a, :c);
},
		'009_swap' => q{
\set a random(1, 100)
BEGIN;
UPDATE codes SET code = -code WHERE id = :a;
UPDATE codes SET code = -code WHERE id = :a;
COMMIT;
},
		'012_verify' => q{
SELECT iheap_verify_strict('codes');
}
	});
is($node->safe_psql('postgres', 'SELECT count(*), count(DISTINCT code) FROM codes'),
	'100|100', 'codes stay unique');
is($node->safe_psql('postgres', q{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM codes c
WHERE (SELECT count(*) FROM codes c2 WHERE c2.code = c.code) <> 1}),
	'0', 'every row found once through its code');
$node->safe_psql('postgres', 'VACUUM codes');
is($node->safe_psql('postgres', 'SELECT tids, marked FROM dmbtree_index_stats(\'codes_code\')'),
	'100|0', 'marked entries cleaned up');

# rows moving to other pages while a snapshot is held (§21 D45): a writer
# keeps the old page locked while it locks the new one, a later page as the
# block order allows, an earlier one only if its lock is free at once.  The
# free space VACUUM leaves all over the table sends rows to pages before and
# after their own; rows leaving pages whose bodies the snapshot holds take
# their bodies to undo.  A deadlock among buffer locks would hang the run:
# the alarm fails the test instead.
$node->safe_psql('postgres', q{
CREATE TABLE mvt (id int PRIMARY KEY, k int, v int, pad text) USING iheap;
CREATE INDEX mvt_k ON mvt USING btree (k);
INSERT INTO mvt SELECT g, g, 100, repeat('t', 100) FROM generate_series(1, 3000) g;
UPDATE mvt SET pad = 't' WHERE id % 3 = 0;
VACUUM mvt;
CREATE FUNCTION check_mvt() RETURNS void LANGUAGE plpgsql AS $$
DECLARE s bigint; c bigint;
BEGIN
  SELECT sum(v), count(*) INTO s, c FROM mvt;
  IF s <> 300000 OR c <> 3000 THEN
    RAISE EXCEPTION 'inconsistent snapshot: sum %, count %', s, c;
  END IF;
END $$;
});
my $old = $node->background_psql('postgres');
$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
my $before = $old->query_safe('SELECT count(*), sum(v), sum(k) FROM mvt');
{
	local $SIG{ALRM} = sub { die "moves under a held snapshot hang: a deadlock?\n" };
	alarm(3 * $PostgreSQL::Test::Utils::timeout_default);
	$node->pgbench(
		'--no-vacuum --client=8 --jobs=4 --transactions=400 --max-tries=100',
		0,
		[qr{processed: 3200/3200}],
		[qr{^$}],
		'moves to pages before and after, a snapshot held',
		{
			'017_move_pair' => q{
\set a random(1, 3000)
\set b random(1, 3000)
BEGIN;
UPDATE mvt SET k = k + 3000, v = v - 1 WHERE id = :a;
UPDATE mvt SET k = k + 3000, v = v + 1 WHERE id = :b;
COMMIT;
},
			'018_grow_move' => q{
\set a random(1, 3000)
\set n random(1, 200)
UPDATE mvt SET k = -k, pad = repeat('g', :n) WHERE id = :a;
},
			'019_abort' => q{
\set a random(1, 3000)
BEGIN;
UPDATE mvt SET k = k + 100000, v = v + 1000 WHERE id = :a;
ROLLBACK;
},
			'020_check' => q{
SELECT check_mvt();
},
			'021_verify' => q{
SELECT iheap_verify_strict('mvt');
}
		});
	alarm(0);
}
is($old->query_safe('SELECT count(*), sum(v), sum(k) FROM mvt'), $before,
	'the held snapshot still sees the rows before the moves');
$old->query_safe('COMMIT');
$old->quit;
is($node->safe_psql('postgres', 'SELECT count(*), sum(v) FROM mvt'),
	'3000|300000', 'moves under a held snapshot keep the sum');
is($node->safe_psql('postgres', q{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM mvt WHERE k > -100000000}), '3000',
	'the moved rows found through their index');
$node->safe_psql('postgres', 'VACUUM mvt');

for my $t (qw(acct hist codes mvt))
{
	is(iheap_verify($node, $t), '', "structure checks of $t after the workloads");
}

$node->stop;
done_testing();
