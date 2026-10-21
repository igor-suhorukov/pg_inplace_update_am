# Random workloads against heap.  A seeded generator drives writer sessions,
# snapshot sessions and a checker, one statement at a time.  Every change goes
# to an iheap table and to its heap twin, the same statement in the same
# transaction, so the twins hold the same rows under every snapshot.  Per
# statement, the rows it returns (RETURNING, locking reads) and its outcome
# (an error's SQLSTATE) must be the same for both twins; REPEATABLE READ
# snapshots must go on seeing what they saw, through every kind of scan; a
# checker compares the twins through every index; the structure checks run
# while transactions are open; and a standby replays it all with
# wal_consistency_checking.  The changes: updates in place, moves (btree
# indexes), key changes through dmbtree indexes, rows growing, shrinking and
# going to TOAST, deletions, inserts, upserts, MERGE, row locks, savepoints
# rolled back, transactions rolled back (eagerly, or left to lazy rollback),
# prepared transactions; between them VACUUM, discard, VACUUM FULL, CLUSTER,
# REINDEX, CHECKPOINT and crashes.
#
# Writers change disjoint rows (id % 3) and unique keys (u % 3), so that no
# session waits for another and the run is a function of the seed:
#
#   IHEAP_RANDOM_SEED   the seed (default: from the clock; printed)
#   IHEAP_RANDOM_STEPS  steps (default 2500)
#
# The statements run go to random_sql.log in the test's log directory.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IheapTest;

my $seed = $ENV{IHEAP_RANDOM_SEED} // (time() ^ ($$ << 8)) % 1000000;
my $steps = $ENV{IHEAP_RANDOM_STEPS} // 2500;
srand($seed);
diag("seed $seed, $steps steps (IHEAP_RANDOM_SEED, IHEAP_RANDOM_STEPS)");

sub rint { return int(rand($_[0])); }
sub chance { return rand() < $_[0]; }
sub pick { return $_[ rint(scalar @_) ]; }

my $node = iheap_node('random', allows_streaming => 1, extra_conf => q{
wal_consistency_checking = 'iheap,dmbtree'
max_connections = 30
});

# Three pairs.  r1: btree indexes, so key changes move rows.  r2: dmbtree
# indexes (the default), with an expression, a predicate with INCLUDE and
# a unique key.  r3: full pages, rows with compact headers.
my @pairs = qw(r1 r2 r3);
my %ddl = (
	r1 => q{
CREATE TABLE TBL (id int PRIMARY KEY, k int, u int, v int, s text, t text) USING TAM;
CREATE INDEX TBL_k ON TBL USING btree (k);
CREATE INDEX TBL_s ON TBL USING btree (s);
},
	r2 => q{
CREATE TABLE TBL (id int PRIMARY KEY, k int, u int, v int, s text, t text) USING TAM;
CREATE INDEX TBL_k ON TBL (k);
CREATE INDEX TBL_ls ON TBL (lower(s));
CREATE INDEX TBL_kv ON TBL (k) INCLUDE (v) WHERE v % 2 = 0;
CREATE UNIQUE INDEX TBL_u ON TBL USING IAM (u);
},
	r3 => q{
CREATE TABLE TBL (id int PRIMARY KEY, k int, u int, v int, s text, t text)
  USING TAM WITH (fillfactor = 100);
CREATE INDEX TBL_k ON TBL (k);
});
for my $p (@pairs)
{
	for my $twin ([ $p, 'iheap', 'dmbtree' ], [ "${p}_h", 'heap', 'btree' ])
	{
		my ($tbl, $tam, $iam) = @$twin;
		my $sql = $ddl{$p};
		$sql =~ s/TBL/$tbl/g;
		$sql =~ s/TAM/$tam/g;
		$sql =~ s/IAM/$iam/g;
		$node->safe_psql('postgres', $sql . qq{
INSERT INTO $tbl SELECT g, g % 97, 3 * g + g % 3, g, 's' || g, NULL
  FROM generate_series(1, 600) g;
});
	}
}
$node->safe_psql('postgres', 'SET iheap.compact_headers = always; VACUUM FULL r3');

# (a slot: replay with page images checked lags behind, and crashes make
# checkpoints)
$node->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('standby')");
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->append_conf('postgresql.conf', "primary_slot_name = 'standby'");
$standby->start;

open(my $log, '>', "${PostgreSQL::Test::Utils::log_path}/random_sql.log")
  or die "could not open the statement log: $!";

# ------------------------------------------------------------------------
# Sessions

my $failed;

sub fail_run
{
	my ($what) = @_;
	$failed //= "$what (seed $seed)";
	print $log "FAILED: $what\n";
}

sub connect_session
{
	my ($s) = @_;
	$s->{psql} = $node->background_psql('postgres', on_error_stop => 0);
	$s->{psql}->query_safe(
		"\\set VERBOSITY sqlstate\nSET client_min_messages = warning",
		verbose => 0);
	$s->{psql}->query_safe('SET iheap.foreground_undo_limit = 0', verbose => 0)
	  if $s->{lazy};
	$s->{xact} = 0;
	$s->{savepoints} = [];
	$s->{baseline} = undef;
}

sub new_session
{
	my ($name, %fields) = @_;
	my $s = { name => $name, %fields };
	connect_session($s);
	return $s;
}

# Run SQL in a session; returns its output and the SQLSTATE of its error.
sub run
{
	my ($s, $sql) = @_;
	print $log "$s->{name}: $sql\n";
	my $out = $s->{psql}->query($sql, verbose => 0);
	my $err = $s->{psql}->{stderr};
	$s->{psql}->{stderr} = '';
	return ($out, undef) if $err eq '';
	return ($out, $1) if $err =~ /ERROR:\s+([0-9A-Z]{5})/;
	fail_run("$s->{name}: unexpected messages: $err");
	return ($out, 'XXXXX');
}

my @writers = map { new_session("w$_", w => $_, next_id => 700, lazy => chance(0.3)) } 0 .. 2;
my @snaps = map { new_session("s$_") } 0 .. 1;
my $checker = new_session('c');
my @prepared;    # [gid, writer]
my %count;

sub reconnect_all
{
	for my $s (@writers, @snaps, $checker)
	{
		eval { $s->{psql}->quit };
		connect_session($s);
	}
}

# ------------------------------------------------------------------------
# Reading the twins

# (name, predicate, columns) of the reads: all rows through each index
my %reads = (
	all => [ 'true', 'id, k, u, v, s, md5(t) AS t' ],
	pkey => [ 'id > 0', 'id, k, u, v, s, md5(t) AS t' ],
	k => [ 'k > -2147483648', 'id, k, u, v, s, md5(t) AS t' ],
	s => [ "s >= ''", 'id, k, u, v, s, md5(t) AS t' ],
	ls => [ "lower(s) >= ''", 'id, k, u, v, s, md5(t) AS t' ],
	kv => [ 'v % 2 = 0 AND k > -2147483648', 'k, v' ],
	u => [ 'u > -2147483648', 'id, u' ]);
my %plans = (
	seq => 'on, off, off, off',
	index => 'off, on, off, off',
	only => 'off, on, off, on',
	bitmap => 'off, off, on, off');

sub read_sql
{
	my ($tbl, $read, $plan) = @_;
	my ($pred, $cols) = @{ $reads{$read} };
	my @on = split /, /, $plans{$plan};
	return "SET LOCAL enable_seqscan = $on[0]; SET LOCAL enable_indexscan = $on[1]; "
	  . "SET LOCAL enable_bitmapscan = $on[2]; SET LOCAL enable_indexonlyscan = $on[3]; "
	  . "SELECT count(*), md5(coalesce(string_agg(x::text, E'\\n' ORDER BY x::text), '')) "
	  . "FROM (SELECT $cols FROM $tbl WHERE $pred) x";
}

# both twins of a pair, read the same way in the session's transaction
sub read_pair
{
	my ($s, $p, $read, $plan) = @_;
	my ($h, $herr) = run($s, read_sql("${p}_h", $read, $plan));
	my ($i, $ierr) = run($s, read_sql($p, $read, $plan));
	if (defined $herr || defined $ierr)
	{
		fail_run("$s->{name}: read $p $read $plan failed: "
			  . ($herr // '-') . ' ' . ($ierr // '-'));
		return undef;
	}
	fail_run("$s->{name}: $p differs from heap, $read by $plan: $i against $h")
	  if $h ne $i;
	return $h;
}

# ------------------------------------------------------------------------
# Writers

# the same statement for both twins; %T stands for the table
sub twin_statement
{
	my ($w, $tmpl, $may_fail) = @_;
	my @res;
	for my $tbl ("$w->{p}_h", $w->{p})
	{
		(my $sql = $tmpl) =~ s/%T/$tbl/g;
		run($w, 'SAVEPOINT f') if $may_fail;
		my ($out, $err) = run($w, $sql);
		if ($may_fail)
		{
			my ($o, $e) = run($w,
				defined $err ? 'ROLLBACK TO SAVEPOINT f' : 'RELEASE SAVEPOINT f');
			fail_run("$w->{name}: savepoint f: $e") if defined $e;
		}
		push @res, [ $out, $err ];
		if (defined $err && $err eq '40001')
		{
			# SSI may see more conflicts for iheap: the transaction goes, on
			# both twins
			$count{serialization_failures}++;
			end_xact($w, 'ROLLBACK');
			return;
		}
		if (defined $err && !$may_fail)
		{
			fail_run("$w->{name}: $err from $sql");
			end_xact($w, 'ROLLBACK');
			return;
		}
	}
	my ($h, $i) = @res;
	if (($h->[1] // '') ne ($i->[1] // '') || $h->[0] ne $i->[0])
	{
		fail_run("$w->{name}: $tmpl: heap gave "
			  . ($h->[1] // $h->[0]) . ', iheap ' . ($i->[1] // $i->[0]));
	}
	$count{ defined $h->[1] ? 'errors_alike' : 'statements' }++;
}

sub returning
{
	my ($dml, $cols) = @_;
	$cols //= 'id, k, u, v, s, md5(t) AS t';
	return "WITH x AS ($dml RETURNING $cols) SELECT count(*), "
	  . "md5(coalesce(string_agg(x::text, ',' ORDER BY x::text), '')) FROM x";
}

sub change
{
	my ($w) = @_;
	my $W = $w->{w};
	my $m = 2 + rint(25);
	my $r = rint($m);
	my $sel = "id % 3 = $W AND id % $m = $r";
	my $R = rint(1000);
	my $kind = rint(15);
	$w->{p} = pick(@pairs);

	if ($kind == 0)
	{
		twin_statement($w, returning("UPDATE %T SET v = v + $R WHERE $sel"));
	}
	elsif ($kind == 1)
	{
		twin_statement($w, returning("UPDATE %T SET k = (k + $R) % 1000 WHERE $sel"));
	}
	elsif ($kind == 2)
	{
		my $n = pick(0, 1, 5, 40, 120, 400);
		twin_statement($w,
			returning("UPDATE %T SET s = repeat(chr(97 + $R % 26), $n) WHERE $sel"));
	}
	elsif ($kind == 3)
	{
		my $n = pick(1, 10, 80, 300);
		twin_statement($w, returning(
			"UPDATE %T SET t = CASE WHEN (id + $R) % 3 > 0 THEN repeat(md5(id::text || '$R'), $n) END WHERE $sel"));
	}
	elsif ($kind == 4)
	{
		# unique in r2: may collide with another of the writer's rows
		twin_statement($w, returning("UPDATE %T SET u = u + 3 * ($R % 7) WHERE $sel"), 1);
	}
	elsif ($kind == 5)
	{
		twin_statement($w, returning(
			"UPDATE %T SET v = v + 1, k = k + 1, s = s || chr(97 + $R % 26) WHERE $sel"));
	}
	elsif ($kind == 6)
	{
		twin_statement($w, returning("DELETE FROM %T WHERE $sel AND (id / 3) % 2 = $R % 2"));
	}
	elsif ($kind == 7)
	{
		# new rows
		my $a = $w->{next_id};
		my $b = $a + rint(30);
		$w->{next_id} = $b + 1;
		# (their unique keys may meet keys others moved: alike for both)
		twin_statement($w, returning(
			"INSERT INTO %T SELECT 3 * g + $W, g % 97, 3 * g + $W, g, 's' || g, NULL FROM generate_series($a, $b) g"), 1);
	}
	elsif ($kind == 8)
	{
		# rows that may exist: a unique violation, alike for both
		my $g = 1 + rint($w->{next_id});
		twin_statement($w, returning(
			"INSERT INTO %T VALUES (3 * $g + $W, $R, 3 * ($g + 100000) + $W, $R, 'again', NULL)"), 1);
	}
	elsif ($kind == 9 || $kind == 10)
	{
		my $a = 1 + rint($w->{next_id});
		my $b = $a + rint(20);
		my $action = $kind == 9
		  ? 'DO UPDATE SET v = t.v + EXCLUDED.v, s = left(t.s || EXCLUDED.s, 300)'
		  : 'DO NOTHING';
		twin_statement($w, returning(
			"INSERT INTO %T AS t SELECT 3 * g + $W, g % 97, 3 * (g + 200000 + $R) + $W, g, 'u' || g, NULL FROM generate_series($a, $b) g ON CONFLICT (id) $action",
			't.id, t.k, t.u, t.v, t.s, md5(t.t) AS t'), 1);
	}
	elsif ($kind == 11)
	{
		my $mode = pick('UPDATE', 'NO KEY UPDATE', 'SHARE', 'KEY SHARE');
		twin_statement($w,
			"SELECT count(*), md5(coalesce(string_agg(x::text, ',' ORDER BY x::text), '')) "
			  . "FROM (SELECT id, k, u, v, s, md5(t) AS t FROM %T WHERE $sel FOR $mode) x");
	}
	elsif ($kind == 12)
	{
		my $a = 1 + rint($w->{next_id});
		my $b = $a + rint(20);
		twin_statement($w, returning(
			"MERGE INTO %T t USING (SELECT 3 * g + $W AS id FROM generate_series($a, $b) g) s ON t.id = s.id "
			  . "WHEN MATCHED AND t.v % 3 = 0 THEN DELETE "
			  . "WHEN MATCHED THEN UPDATE SET v = t.v + 7, k = t.k + 1 "
			  . "WHEN NOT MATCHED THEN INSERT (id, k, u, v, s) VALUES (s.id, s.id % 97, 3 * (s.id + 300000) + $W, s.id, 'm')",
			't.id, t.k, t.u, t.v, t.s, md5(t.t) AS t'), 1);
	}
	elsif ($kind == 13)
	{
		# the transaction's own view of its rows, through some index
		my $read = pick(qw(pkey k));
		my $plan = pick(keys %plans);
		my ($pred) = @{ $reads{$read} };
		twin_statement($w,
			"SELECT count(*), md5(coalesce(string_agg(x::text, ',' ORDER BY x::text), '')) "
			  . "FROM (SELECT id, k, u, v, s, md5(t) AS t FROM %T WHERE id % 3 = $W AND $pred) x");
	}
	else
	{
		twin_statement($w, returning("UPDATE %T SET v = v WHERE $sel"));
	}
	$count{"kind$kind"}++;
}

# end the writer's transaction; false if it failed, which only SSI may make
# it do at COMMIT or PREPARE (the transaction then goes, for both twins)
sub end_xact
{
	my ($w, $how) = @_;
	my ($out, $err) = run($w, $how);
	$w->{xact} = 0;
	$w->{savepoints} = [];
	if (defined $err)
	{
		fail_run("$w->{name}: $how failed: $err") if $err ne '40001';
		$count{serialization_failures}++;
		return 0;
	}
	$count{$how =~ /^(\w+)/ ? lc $1 : $how}++;
	return 1;
}

sub writer_step
{
	my ($w) = @_;
	return if $w->{prepared};
	if (!$w->{xact})
	{
		my $iso = pick('READ COMMITTED', 'READ COMMITTED', 'READ COMMITTED',
			'REPEATABLE READ', 'SERIALIZABLE');
		my ($out, $err) = run($w, "BEGIN ISOLATION LEVEL $iso");
		fail_run("$w->{name}: BEGIN failed: $err") if defined $err;
		$w->{xact} = 1;
	}
	my $x = rand();
	if ($x < 0.70)
	{
		change($w);
	}
	elsif ($x < 0.75)
	{
		my $sp = 'sp' . ++$w->{nsp};
		my ($out, $err) = run($w, "SAVEPOINT $sp");
		fail_run("$w->{name}: SAVEPOINT failed: $err") if defined $err;
		push @{ $w->{savepoints} }, $sp;
	}
	elsif ($x < 0.79 && @{ $w->{savepoints} })
	{
		# ROLLBACK TO keeps the savepoint, RELEASE does not; both end the
		# later ones
		my $n = rint(scalar @{ $w->{savepoints} });
		my $sp = $w->{savepoints}[$n];
		my $rollback = chance(0.7);
		my ($out, $err) = run($w,
			$rollback ? "ROLLBACK TO SAVEPOINT $sp" : "RELEASE SAVEPOINT $sp");
		fail_run("$w->{name}: savepoint $sp: $err") if defined $err;
		splice(@{ $w->{savepoints} }, $rollback ? $n + 1 : $n);
		$count{savepoints}++;
	}
	elsif ($x < 0.92)
	{
		end_xact($w, 'COMMIT');
	}
	elsif ($x < 0.98 || @prepared >= 5)
	{
		end_xact($w, 'ROLLBACK');
	}
	else
	{
		my $gid = "g$w->{name}_" . ++$count{prepared};
		if (end_xact($w, "PREPARE TRANSACTION '$gid'"))
		{
			$w->{prepared} = $gid;
			push @prepared, [ $gid, $w ];
		}
	}
}

sub resolve_prepared
{
	my $n = rint(scalar @prepared);
	my ($gid, $w) = @{ splice(@prepared, $n, 1) };
	my $how = chance(0.6) ? 'COMMIT' : 'ROLLBACK';
	my ($out, $err) = run($checker, "$how PREPARED '$gid'");
	fail_run("$how PREPARED '$gid': $err") if defined $err;
	$w->{prepared} = undef;
	$count{"prepared_" . lc $how}++;
}

# ------------------------------------------------------------------------
# Snapshots, the checker, maintenance

sub snapshot_step
{
	my ($s) = @_;
	if (!$s->{baseline})
	{
		run($s, 'BEGIN ISOLATION LEVEL REPEATABLE READ');
		$s->{baseline} = {};
		for my $p (@pairs)
		{
			$s->{baseline}{$p} = read_pair($s, $p, 'all', 'seq');
		}
		$count{snapshots}++;
	}
	elsif (chance(0.15))
	{
		run($s, 'COMMIT');
		$s->{baseline} = undef;
	}
	else
	{
		# the same rows, read another way
		my $p = pick(@pairs);
		my $read = pick(qw(pkey k));
		$read = pick(qw(s ls)) if chance(0.3);
		$read = 's' if $read eq 'ls' && $p ne 'r2';
		$read = 'ls' if $read eq 's' && $p eq 'r2';
		$read = 'pkey' if $p eq 'r3' && $read ne 'k';
		my $plan = pick(qw(seq index bitmap));
		my $h = read_pair($s, $p, $read, $plan);
		fail_run("$s->{name}: $p changed under its snapshot ($read by $plan)")
		  if defined $h && $h ne $s->{baseline}{$p};
		$count{snapshot_reads}++;
	}
}

my %read_of = (
	r1 => [qw(all pkey k s)],
	r2 => [qw(all pkey k ls kv u)],
	r3 => [qw(all pkey k)]);

sub check_all
{
	run($checker, 'BEGIN ISOLATION LEVEL REPEATABLE READ');
	for my $p (@pairs)
	{
		my %seen;
		for my $read (@{ $read_of{$p} })
		{
			for my $plan ($read eq 'all' ? ('seq') : qw(index bitmap only))
			{
				next if $plan eq 'only' && $read ne 'kv' && $read ne 'u';
				my $h = read_pair($checker, $p, $read, $plan);
				# every way of reading all rows gives the same
				if (defined $h && $read ne 'kv' && $read ne 'u')
				{
					$seen{$h}++;
				}
			}
		}
		fail_run("$p: its reads disagree with each other") if keys %seen > 1;
	}
	run($checker, 'COMMIT');
	$count{checks}++;
}

sub verify_all
{
	for my $p (@pairs)
	{
		my ($out, $err) = run($checker,
			"SELECT coalesce(string_agg(x, ' / '), 'ok') FROM iheap_verify('$p') x");
		fail_run("structure checks of $p: " . ($err // $out))
		  if defined $err || $out ne 'ok';
	}
	$count{verifications}++;
}

# end every transaction and snapshot: for operations that take an
# exclusive lock
sub barrier
{
	for my $w (@writers)
	{
		end_xact($w, chance(0.8) ? 'COMMIT' : 'ROLLBACK') if $w->{xact};
	}
	resolve_prepared() while @prepared;
	for my $s (@snaps)
	{
		if ($s->{baseline})
		{
			run($s, 'COMMIT');
			$s->{baseline} = undef;
		}
	}
}

sub maintenance
{
	my $x = rand();
	my $p = pick(@pairs);
	my $sql;
	if ($x < 0.35)
	{
		$sql = "VACUUM $p";
	}
	elsif ($x < 0.55)
	{
		$sql = "SELECT iheap_discard('$p')";
	}
	elsif ($x < 0.65)
	{
		$sql = 'CHECKPOINT';
	}
	elsif ($x < 0.90)
	{
		barrier();
		$sql = pick(
			"VACUUM FULL $p",
			"CLUSTER $p USING ${p}_pkey",
			"REINDEX TABLE $p",
			'SET iheap.compact_headers = always; VACUUM FULL r3; RESET iheap.compact_headers');
	}
	else
	{
		# a crash: open transactions go, prepared ones stay
		$node->stop('immediate');
		$node->start;
		reconnect_all();
		$count{crashes}++;
		return;
	}
	my ($out, $err) = run($checker, $sql);
	fail_run("$sql: $err") if defined $err;
	$count{maintenance}++;
}

# ------------------------------------------------------------------------
# The run

for my $step (1 .. $steps)
{
	last if defined $failed;
	print $log "-- step $step\n";
	my $x = rand();
	if ($x < 0.70)
	{
		writer_step(pick(@writers));
	}
	elsif ($x < 0.85)
	{
		snapshot_step(pick(@snaps));
	}
	elsif ($x < 0.90 && @prepared)
	{
		resolve_prepared();
	}
	elsif ($x < 0.95)
	{
		check_all();
	}
	elsif ($x < 0.985)
	{
		maintenance();
	}
	else
	{
		verify_all();
	}
}

barrier() unless defined $failed;
check_all() unless defined $failed;
verify_all() unless defined $failed;
note(join(', ', map { "$_ $count{$_}" } sort keys %count));
ok(!defined $failed, "$steps random steps, the twins alike")
  or diag($failed);

# the standby replayed it all
$node->wait_for_catchup($standby);
for my $p (@pairs)
{
	is(iheap_digest($standby, $p), iheap_digest($node, $p),
		"$p on the standby as on the primary");
	is(iheap_digest($standby, $p), iheap_digest($standby, "${p}_h"),
		"$p on the standby matches heap");
	is(iheap_verify($standby, $p), '', "structure checks of $p on the standby");
}
unlike(slurp_file($standby->logfile), qr/inconsistent page found/,
	'WAL consistency checks passed');

for my $s (@writers, @snaps, $checker)
{
	eval { $s->{psql}->quit };
}
close($log);
$standby->stop;
$node->stop;
done_testing();
