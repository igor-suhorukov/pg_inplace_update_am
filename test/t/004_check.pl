# Corruption the structure checks must report: iheap_check() as rows,
# dmbtree_check() and dmbtree_parent_check() as errors.  Pages are changed
# behind the server's back while it is stopped (no data checksums, so that
# the pages can still be read).
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IheapTest;

my $node = iheap_node('check', no_data_checksums => 1);
my $BLCKSZ = $node->safe_psql('postgres', 'SHOW block_size');

sub relfile
{
	my ($rel) = @_;
	return $node->data_dir . '/'
	  . $node->safe_psql('postgres', "SELECT pg_relation_filepath('$rel')");
}

sub read_bytes
{
	my ($file, $offset, $len) = @_;
	open(my $fh, '<', $file) or die "open $file: $!";
	binmode $fh;
	sysseek($fh, $offset, 0) or die "seek $file: $!";
	my $buf;
	sysread($fh, $buf, $len) == $len or die "read $file: $!";
	close $fh;
	return $buf;
}

sub write_bytes
{
	my ($file, $offset, $bytes) = @_;
	open(my $fh, '+<', $file) or die "open $file: $!";
	binmode $fh;
	sysseek($fh, $offset, 0) or die "seek $file: $!";
	syswrite($fh, $bytes) == length($bytes) or die "write $file: $!";
	close $fh;
}

# (block, offset) of the row with the given id
sub tid_of
{
	my ($table, $id) = @_;
	my $tid = $node->safe_psql('postgres', "SELECT ctid FROM $table WHERE id = $id");
	$tid =~ /\((\d+),(\d+)\)/ or die "no ctid for $table $id";
	return ($1, $2);
}

# file offset of the tuple of a row, and of its line pointer
sub tuple_offset
{
	my ($table, $id) = @_;
	my ($blk, $off) = tid_of($table, $id);
	my $lp_off = $node->safe_psql('postgres',
		"SELECT lp_off FROM iheap_page_items('$table', $blk) WHERE lp = $off");
	return ($blk * $BLCKSZ + $lp_off, $blk * $BLCKSZ + 24 + 4 * ($off - 1), $blk, $off);
}

# the items of a btree-format leaf page: line pointer, place, TID, int4 key
sub index_items
{
	my ($page) = @_;
	my $lower = unpack('v', substr($page, 12, 2));
	my @items;
	for my $i (1 .. ($lower - 24) / 4)
	{
		my $w = unpack('V', substr($page, 24 + 4 * ($i - 1), 4));
		next unless (($w >> 15) & 3) == 1;
		my $off = $w & 0x7fff;
		my ($hi, $lo, $posid) = unpack('vvv', substr($page, $off, 6));
		push @items, {
			lp => $i, off => $off, block => ($hi << 16) | $lo,
			posid => $posid, key => unpack('l<', substr($page, $off + 8, 4)) };
	}
	return @items;
}

sub problems
{
	my ($table, $setting) = @_;
	return $node->safe_psql('postgres', ($setting // '') . qq{
SELECT string_agg(blkno || ',' || coalesce(offnum::text, '-') || ': ' || msg, E'\\n')
FROM iheap_check('$table')});
}

sub index_check_error
{
	my ($call) = @_;
	my ($ret, $out, $err) = $node->psql('postgres', "SELECT $call");
	return $ret == 0 ? '' : $err;
}

# ------------------------------------------------------------------------
# healthy tables first
# ------------------------------------------------------------------------
my @plain = qw(c_natts c_flags c_urec c_lp c_undo c_meta c_vm);
for my $t (@plain)
{
	$node->safe_psql('postgres', qq{
CREATE TABLE $t (id int, s text) USING iheap;
INSERT INTO $t SELECT g, 'row ' || g FROM generate_series(1, 20) g;
});
}
$node->safe_psql('postgres', 'VACUUM c_vm');
for my $t (qw(c_mark c_unused c_order))
{
	$node->safe_psql('postgres', qq{
CREATE TABLE $t (id int, k int) USING iheap;
CREATE INDEX ${t}_k ON $t (k);
INSERT INTO $t SELECT g, g FROM generate_series(1, 100) g;
});
}
$node->safe_psql('postgres', 'UPDATE c_mark SET k = k + 1000 WHERE id = 5');
$node->safe_psql('postgres', q{
CREATE TABLE c_big (id int, k int, pad text) USING iheap;
INSERT INTO c_big SELECT g, g, repeat('p', 30) FROM generate_series(1, 300000) g;
CREATE INDEX c_big_k ON c_big (k);
});

for my $t (@plain, qw(c_mark c_unused c_order c_big))
{
	is(iheap_verify($node, $t), '', "$t is healthy");
}
is(problems('c_big', 'SET maintenance_work_mem = 64;'), '',
	'c_big is healthy, checked in ranges of blocks');

# where to corrupt
my %file = map { $_ => relfile($_) } (@plain, qw(c_mark c_unused c_big),
	qw(c_mark_k c_order_k));
my ($natts_at, undef, $natts_blk, $natts_off) = tuple_offset('c_natts', 3);
my ($flags_at) = tuple_offset('c_flags', 4);
my ($urec_at) = tuple_offset('c_urec', 5);
my (undef, $lp_at) = tuple_offset('c_lp', 6);
my ($vm_at) = tuple_offset('c_vm', 7);
my (undef, $unused_lp, $unused_blk, $unused_off) = tuple_offset('c_unused', 7);
my (undef, $big1_lp, $big1_blk, $big1_off) = tuple_offset('c_big', 1);
my (undef, $big2_lp, $big2_blk, $big2_off) = tuple_offset('c_big', 299999);
my ($mark_blk, $mark_off) = tid_of('c_mark', 5);
cmp_ok($big2_blk, '>', 2 * 1560, 'c_big spans several ranges of blocks');

$node->stop;

# ------------------------------------------------------------------------
# corrupt
# ------------------------------------------------------------------------

# a tuple with more attributes than its relation
write_bytes($file{c_natts}, $natts_at, pack('v', 0x07ff));

# an infomask bit iheap does not define
my $mask = unpack('v', read_bytes($file{c_flags}, $flags_at + 2, 2));
write_bytes($file{c_flags}, $flags_at + 2, pack('v', $mask | 0x8000));

# an undo pointer beyond the end of the stream (7 bytes, little endian)
my $urp = 1000000 * $BLCKSZ + 100;
write_bytes($file{c_urec}, $urec_at + 5,
	join('', map { chr(($urp >> (8 * $_)) & 0xff) } 0 .. 6));

# a line pointer reaching beyond the page
my $lpw = unpack('V', read_bytes($file{c_lp}, $lp_at, 4));
write_bytes($file{c_lp}, $lp_at, pack('V', ($lpw & 0x1ffff) | (8000 << 17)));

# an undo record of an unknown type: the first record of the first undo page
{
	my $size = -s $file{c_undo};
	my $done = 0;
	for (my $b = 1; $b * $BLCKSZ < $size && !$done; $b++)
	{
		next unless unpack('v', read_bytes($file{c_undo}, ($b + 1) * $BLCKSZ - 8, 2)) == 0x6903;
		write_bytes($file{c_undo}, $b * $BLCKSZ + 48 + 24, chr(99));
		$done = 1;
	}
	ok($done, 'found an undo page to corrupt');
}

# the metapage: a discard pointer inside a page, an extent of 3 pages
{
	my ($lo, $hi) = unpack('VV', read_bytes($file{c_meta}, 32, 8));
	write_bytes($file{c_meta}, 32, pack('VV', $lo + 1, $hi));
	write_bytes($file{c_meta}, 76, pack('V', 3));
}

# a deleted row on an all-visible page
$mask = unpack('v', read_bytes($file{c_vm}, $vm_at + 2, 2));
write_bytes($file{c_vm}, $vm_at + 2, pack('v', $mask | 0x0040));

# a row's line pointer made unused while index entries point to it
write_bytes($file{c_unused}, $unused_lp, pack('V', 0));
write_bytes($file{c_big}, $big1_lp, pack('V', 0));
write_bytes($file{c_big}, $big2_lp, pack('V', 0));

# dmbtree: the delete-mark taken off the entry of a row's old key
{
	my $page = read_bytes($file{c_mark_k}, $BLCKSZ, $BLCKSZ);
	my ($item) = grep { $_->{key} == 5 } index_items($page);
	ok($item && $item->{block} == $mark_blk
		  && ($item->{posid} & 0x7fff) == $mark_off && ($item->{posid} & 0x8000),
		'the old key\'s entry is delete-marked');
	write_bytes($file{c_mark_k}, $BLCKSZ + $item->{off} + 4,
		pack('v', $item->{posid} & 0x7fff));
}

# dmbtree: an entry's key out of order
{
	my $page = read_bytes($file{c_order_k}, $BLCKSZ, $BLCKSZ);
	my ($item) = grep { $_->{key} == 10 } index_items($page);
	write_bytes($file{c_order_k}, $BLCKSZ + $item->{off} + 8, pack('l<', 1000000));
}

$node->start;

# ------------------------------------------------------------------------
# and see it reported
# ------------------------------------------------------------------------
like(problems('c_natts'),
	qr/^$natts_blk,$natts_off: tuple has 2047 attributes, the relation 2$/m,
	'too many attributes');
like(problems('c_flags'), qr/: tuple has unknown flags 8000$/m, 'unknown flags');
like(problems('c_urec'), qr/: undo pointer \d+ is beyond the end of the undo stream$/m,
	'undo pointer beyond the stream');
like(problems('c_lp'), qr/: line pointer to \d+ of 8000 bytes is outside the tuple space/m,
	'line pointer beyond the page');
like(problems('c_undo'), qr/: undo record \d+ has type 99$/m, 'undo record type');
my $meta = problems('c_meta');
like($meta, qr/^0,-: discard pointer \d+ is not the start of an undo page$/m,
	'discard pointer inside a page');
like($meta, qr/^0,-: extent at block \d+ has 3 pages, not a power of two/m,
	'extent size');
like(problems('c_vm'), qr/: page is all-visible, but the tuple is deleted$/m,
	'deleted row on an all-visible page');

like(problems('c_unused'),
	qr/^$unused_blk,$unused_off: index "c_unused_k" \(block 1\) points to an unused line pointer$/m,
	'index entry pointing to an unused line pointer');
like(index_check_error("dmbtree_check('c_unused_k', checkmarks => true)"),
	qr/index "c_unused_k" has an entry pointing to an unused line pointer of table "c_unused"/,
	'dmbtree_check sees the entry pointing to an unused line pointer');

for my $setting ('', 'SET maintenance_work_mem = 64;')
{
	my $p = problems('c_big', $setting);
	like($p, qr/^$big1_blk,$big1_off: index "c_big_k" \(block \d+\) points to an unused line pointer$/m,
		"first range of blocks ($setting)");
	like($p, qr/^$big2_blk,$big2_off: index "c_big_k" \(block \d+\) points to an unused line pointer$/m,
		"last range of blocks ($setting)");
}

is(problems('c_mark'), '', 'the table under the unmarked entry is fine');
is(index_check_error("dmbtree_check('c_mark_k', true, true)"), '',
	'the unmarked entry passes without checkmarks');
like(index_check_error("dmbtree_check('c_mark_k', checkmarks => true)"),
	qr/unmarked entry of index "c_mark_k" does not match the newest committed version of its row/,
	'checkmarks finds the entry that lost its mark');
like(index_check_error("dmbtree_parent_check('c_mark_k', checkmarks => true)"),
	qr/unmarked entry of index "c_mark_k" does not match/,
	'so does dmbtree_parent_check');

like(index_check_error("dmbtree_check('c_order_k')"),
	qr/item order invariant violated for index "c_order_k"/, 'entries out of order');
like(index_check_error("dmbtree_parent_check('c_order_k')"),
	qr/item order invariant violated for index "c_order_k"/,
	'entries out of order, parent check');

$node->stop;
done_testing();
