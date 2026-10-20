-- TPC-H: primary keys, and the foreign-key indexes the correlated
-- subqueries of q17 and q20 (lineitem by part and supplier) and the anti
-- join of q22 (orders by customer) need on PostgreSQL, which does not
-- decorrelate them.
ALTER TABLE region ADD PRIMARY KEY (r_regionkey);
ALTER TABLE nation ADD PRIMARY KEY (n_nationkey);
ALTER TABLE part ADD PRIMARY KEY (p_partkey);
ALTER TABLE supplier ADD PRIMARY KEY (s_suppkey);
ALTER TABLE partsupp ADD PRIMARY KEY (ps_partkey, ps_suppkey);
ALTER TABLE customer ADD PRIMARY KEY (c_custkey);
ALTER TABLE orders ADD PRIMARY KEY (o_orderkey);
ALTER TABLE lineitem ADD PRIMARY KEY (l_orderkey, l_linenumber);
CREATE INDEX lineitem_part_supp ON lineitem (l_partkey, l_suppkey);
CREATE INDEX orders_cust ON orders (o_custkey);
