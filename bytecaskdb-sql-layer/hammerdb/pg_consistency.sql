-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 Gustavo Amigo
--
-- pg_consistency.sql — the benchmark specification's consistency conditions
-- 1, 8 and 9 (clause 3.3.2), in PostgreSQL's SQL, for the driver's run on
-- PostgreSQL 17 (docs/server_design.md, *The same script against
-- PostgreSQL 17*). The harness checks the same conditions on the layer
-- (sql_layer_tprocc check). Each query returns the rows that break its
-- condition: the run is consistent when all three return none.
--
-- psql -X -q -At -v ON_ERROR_STOP=1 -f pg_consistency.sql

-- 1: each warehouse's W_YTD is the sum of its districts' D_YTD.
SELECT 'condition 1', w_id, w_ytd FROM warehouse w
WHERE w_ytd <> (SELECT sum(d_ytd) FROM district WHERE d_w_id = w.w_id);

-- 8: each warehouse's W_YTD is the sum of its history's H_AMOUNT.
SELECT 'condition 8', w_id, w_ytd FROM warehouse w
WHERE w_ytd <> (SELECT coalesce(sum(h_amount), 0) FROM history WHERE h_w_id = w.w_id);

-- 9: each district's D_YTD is the sum of its history's H_AMOUNT.
SELECT 'condition 9', d_w_id, d_id, d_ytd FROM district d
WHERE d_ytd <> (SELECT coalesce(sum(h_amount), 0) FROM history WHERE h_w_id = d.d_w_id AND h_d_id = d.d_id);
