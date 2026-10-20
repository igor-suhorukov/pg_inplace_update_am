-- TPC-DS: the primary keys of the specification (§2.2-2.3), dimensions by
-- their surrogate key, sales and returns by item and ticket or order.
ALTER TABLE call_center ADD PRIMARY KEY (cc_call_center_sk);
ALTER TABLE catalog_page ADD PRIMARY KEY (cp_catalog_page_sk);
ALTER TABLE customer ADD PRIMARY KEY (c_customer_sk);
ALTER TABLE customer_address ADD PRIMARY KEY (ca_address_sk);
ALTER TABLE customer_demographics ADD PRIMARY KEY (cd_demo_sk);
ALTER TABLE date_dim ADD PRIMARY KEY (d_date_sk);
ALTER TABLE household_demographics ADD PRIMARY KEY (hd_demo_sk);
ALTER TABLE income_band ADD PRIMARY KEY (ib_income_band_sk);
ALTER TABLE item ADD PRIMARY KEY (i_item_sk);
ALTER TABLE promotion ADD PRIMARY KEY (p_promo_sk);
ALTER TABLE reason ADD PRIMARY KEY (r_reason_sk);
ALTER TABLE ship_mode ADD PRIMARY KEY (sm_ship_mode_sk);
ALTER TABLE store ADD PRIMARY KEY (s_store_sk);
ALTER TABLE time_dim ADD PRIMARY KEY (t_time_sk);
ALTER TABLE warehouse ADD PRIMARY KEY (w_warehouse_sk);
ALTER TABLE web_page ADD PRIMARY KEY (wp_web_page_sk);
ALTER TABLE web_site ADD PRIMARY KEY (web_site_sk);
ALTER TABLE store_sales ADD PRIMARY KEY (ss_item_sk, ss_ticket_number);
ALTER TABLE store_returns ADD PRIMARY KEY (sr_item_sk, sr_ticket_number);
ALTER TABLE catalog_sales ADD PRIMARY KEY (cs_item_sk, cs_order_number);
ALTER TABLE catalog_returns ADD PRIMARY KEY (cr_item_sk, cr_order_number);
ALTER TABLE web_sales ADD PRIMARY KEY (ws_item_sk, ws_order_number);
ALTER TABLE web_returns ADD PRIMARY KEY (wr_item_sk, wr_order_number);
ALTER TABLE inventory ADD PRIMARY KEY (inv_date_sk, inv_item_sk, inv_warehouse_sk);
