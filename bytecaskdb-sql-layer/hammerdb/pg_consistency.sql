-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 Gustavo Amigo
--
-- pg_consistency.sql — the benchmark specification's consistency conditions
-- 1 to 9 (clause 3.3.2), in PostgreSQL's SQL, for the driver's run on
-- PostgreSQL 17 (docs/server_design.md, *The same script against
-- PostgreSQL 17*). The harness checks the same conditions, with the same
-- definitions, on the layer (sql_layer_tprocc check). Each query returns the
-- rows that break its condition: the run is consistent when all nine return
-- none.
--
-- psql -X -q -At -v ON_ERROR_STOP=1 -f pg_consistency.sql

-- 1: each warehouse's W_YTD is the sum of its districts' D_YTD.
SELECT 'condition 1', w_id, w_ytd FROM warehouse w
WHERE w_ytd <> (SELECT sum(d_ytd) FROM district WHERE d_w_id = w.w_id);

-- Per district: its orders' highest o_id and o_ol_cnt sum, its order lines,
-- and its new_order rows' range and count. A district without orders has
-- max(o_id) 0.
CREATE TEMPORARY VIEW district_orders AS
SELECT d.d_w_id AS w_id, d.d_id, d.d_next_o_id,
       coalesce(o.max_o, 0) AS max_o, coalesce(o.ol_cnt_sum, 0) AS ol_cnt_sum,
       coalesce(ol.lines, 0) AS lines, n.min_no, n.max_no, coalesce(n.news, 0) AS news
FROM district d
LEFT JOIN (SELECT o_w_id, o_d_id, max(o_id) AS max_o, sum(o_ol_cnt) AS ol_cnt_sum
           FROM orders GROUP BY o_w_id, o_d_id) o ON o.o_w_id = d.d_w_id AND o.o_d_id = d.d_id
LEFT JOIN (SELECT ol_w_id, ol_d_id, count(*) AS lines
           FROM order_line GROUP BY ol_w_id, ol_d_id) ol ON ol.ol_w_id = d.d_w_id AND ol.ol_d_id = d.d_id
LEFT JOIN (SELECT no_w_id, no_d_id, min(no_o_id) AS min_no, max(no_o_id) AS max_no, count(*) AS news
           FROM new_order GROUP BY no_w_id, no_d_id) n ON n.no_w_id = d.d_w_id AND n.no_d_id = d.d_id;

-- Per order: its carrier, its o_ol_cnt, its lines and those delivered, and
-- whether it has a new_order row.
CREATE TEMPORARY VIEW order_lines AS
SELECT o.o_w_id AS w_id, o.o_d_id AS d_id, o.o_id, o.o_carrier_id IS NOT NULL AS carried, o.o_ol_cnt,
       coalesce(ol.lines, 0) AS lines, coalesce(ol.delivered, 0) AS delivered,
       EXISTS (SELECT 1 FROM new_order WHERE no_w_id = o.o_w_id AND no_d_id = o.o_d_id AND no_o_id = o.o_id)
         AS undelivered
FROM orders o
LEFT JOIN (SELECT ol_w_id, ol_d_id, ol_o_id, count(*) AS lines, count(ol_delivery_d) AS delivered
           FROM order_line GROUP BY ol_w_id, ol_d_id, ol_o_id) ol
  ON ol.ol_w_id = o.o_w_id AND ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id;

-- 2: each district's d_next_o_id - 1 is its max(o_id), and its
-- max(no_o_id) too when it has new_order rows.
SELECT 'condition 2', w_id, d_id, d_next_o_id, max_o, max_no FROM district_orders
WHERE d_next_o_id - 1 <> max_o OR max_no <> max_o;

-- 3: each district's new_order rows are contiguous: max(no_o_id) -
-- min(no_o_id) + 1 rows.
SELECT 'condition 3', w_id, d_id, min_no, max_no, news FROM district_orders
WHERE max_no - min_no + 1 <> news;

-- 4: each district's o_ol_cnt sums to its order lines.
SELECT 'condition 4', w_id, d_id, ol_cnt_sum, lines FROM district_orders
WHERE ol_cnt_sum <> lines;

-- 5: an order has a carrier exactly when it has no new_order row.
SELECT 'condition 5', w_id, d_id, o_id, carried, undelivered FROM order_lines
WHERE carried = undelivered;

-- 6: each order's o_ol_cnt is its line count.
SELECT 'condition 6', w_id, d_id, o_id, o_ol_cnt, lines FROM order_lines
WHERE o_ol_cnt <> lines;

-- 7: an order's lines are all delivered when it has a carrier, and none
-- are when it has not.
SELECT 'condition 7', w_id, d_id, o_id, carried, delivered, lines FROM order_lines
WHERE delivered <> CASE WHEN carried THEN lines ELSE 0 END;

-- 8: each warehouse's W_YTD is the sum of its history's H_AMOUNT.
SELECT 'condition 8', w_id, w_ytd FROM warehouse w
WHERE w_ytd <> (SELECT coalesce(sum(h_amount), 0) FROM history WHERE h_w_id = w.w_id);

-- 9: each district's D_YTD is the sum of its history's H_AMOUNT.
SELECT 'condition 9', d_w_id, d_id, d_ytd FROM district d
WHERE d_ytd <> (SELECT coalesce(sum(h_amount), 0) FROM history WHERE h_w_id = d.d_w_id AND h_d_id = d.d_id);
