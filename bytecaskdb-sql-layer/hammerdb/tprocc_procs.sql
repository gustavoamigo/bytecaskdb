-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 Gustavo Amigo
--
-- TPROC-C's five transactions as PL/pgSQL procedures (docs/procedures_design.md,
-- *Phases*, 4c), for hammerdb/bcdboltp.tcl. Their logic is HammerDB 6.0's
-- PostgreSQL stored procedures' (src/postgresql/pgoltp.tcl): New-Order draws
-- its lines in the procedure, Payment and Order-Status pick the customer by
-- last name, Delivery runs every district. Their SQL is the layer's: the
-- statements of harness/tprocc.cppm, which the proof suite checks against
-- PostgreSQL 17, so the same file runs on PostgreSQL 17 unchanged.
--
-- Every variable is prefixed (p_ an argument, v_ a local): a name that is
-- both a variable and a column is 42702.

-- New-Order (clause 2.4.2). Draws each line's item, supply warehouse and
-- quantity, as HammerDB's procedure does; the last line's item is unused 1%
-- of the time (clause 2.4.1.4), and its SELECT … INTO STRICT then fails the
-- call with P0002, which rolls it back. A line's amount carries the taxes
-- and the discount, as HammerDB's does.
CREATE PROCEDURE neword(
  p_w_id INTEGER, p_max_w_id INTEGER, p_d_id INTEGER, p_c_id INTEGER, p_ol_cnt INTEGER, p_max_i_id INTEGER,
  INOUT p_c_discount NUMERIC(4, 4), INOUT p_c_last VARCHAR(16), INOUT p_c_credit VARCHAR(2),
  INOUT p_d_tax NUMERIC(4, 4), INOUT p_w_tax NUMERIC(4, 4), INOUT p_o_id INTEGER, p_tstamp TIMESTAMP)
LANGUAGE plpgsql AS $$
DECLARE
  v_all_local SMALLINT := 1;
  v_rbk INTEGER;
  v_i_id INTEGER;
  v_supply_w_id INTEGER;
  v_quantity SMALLINT;
  v_price NUMERIC(5, 2);
  v_i_name VARCHAR(24);
  v_i_data VARCHAR(50);
  v_stock SMALLINT;
  v_s_data VARCHAR(50);
  v_d01 VARCHAR(24); v_d02 VARCHAR(24); v_d03 VARCHAR(24); v_d04 VARCHAR(24); v_d05 VARCHAR(24);
  v_d06 VARCHAR(24); v_d07 VARCHAR(24); v_d08 VARCHAR(24); v_d09 VARCHAR(24); v_d10 VARCHAR(24);
  v_dist VARCHAR(24);
  v_amount NUMERIC(6, 2);
BEGIN
  SELECT c_discount, c_last, c_credit, w_tax INTO p_c_discount, p_c_last, p_c_credit, p_w_tax
    FROM customer, warehouse WHERE w_id = p_w_id AND c_w_id = p_w_id AND c_d_id = p_d_id AND c_id = p_c_id;
  SELECT d_next_o_id, d_tax INTO p_o_id, p_d_tax FROM district WHERE d_id = p_d_id AND d_w_id = p_w_id FOR UPDATE;
  UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_id = p_d_id AND d_w_id = p_w_id;
  v_rbk := random(1, 100);
  FOR v_number IN 1..p_ol_cnt LOOP
    IF v_number = p_ol_cnt AND v_rbk = 1 THEN
      v_i_id := p_max_i_id + 1;
    ELSE
      v_i_id := random(1, p_max_i_id);
    END IF;
    -- 1% of lines from another warehouse (clause 2.4.1.5.2).
    v_supply_w_id := p_w_id;
    IF p_max_w_id > 1 AND random(1, 100) = 1 THEN
      v_all_local := 0;
      v_supply_w_id := p_w_id + random(1, p_max_w_id - 1);
      IF v_supply_w_id > p_max_w_id THEN
        v_supply_w_id := v_supply_w_id - p_max_w_id;
      END IF;
    END IF;
    v_quantity := random(1, 10);
    SELECT i_price, i_name, i_data INTO STRICT v_price, v_i_name, v_i_data FROM item WHERE i_id = v_i_id;
    SELECT s_quantity, s_data, s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05, s_dist_06, s_dist_07, s_dist_08,
           s_dist_09, s_dist_10
      INTO v_stock, v_s_data, v_d01, v_d02, v_d03, v_d04, v_d05, v_d06, v_d07, v_d08, v_d09, v_d10
      FROM stock WHERE s_i_id = v_i_id AND s_w_id = v_supply_w_id;
    v_stock := v_stock - v_quantity;
    IF v_stock < 10 THEN
      v_stock := v_stock + 91;  -- restocked (clause 2.4.2.2)
    END IF;
    UPDATE stock SET s_quantity = v_stock WHERE s_i_id = v_i_id AND s_w_id = v_supply_w_id;
    v_amount := v_quantity * v_price * (1 + p_w_tax + p_d_tax) * (1 - p_c_discount);
    IF p_d_id = 1 THEN v_dist := v_d01;
    ELSIF p_d_id = 2 THEN v_dist := v_d02;
    ELSIF p_d_id = 3 THEN v_dist := v_d03;
    ELSIF p_d_id = 4 THEN v_dist := v_d04;
    ELSIF p_d_id = 5 THEN v_dist := v_d05;
    ELSIF p_d_id = 6 THEN v_dist := v_d06;
    ELSIF p_d_id = 7 THEN v_dist := v_d07;
    ELSIF p_d_id = 8 THEN v_dist := v_d08;
    ELSIF p_d_id = 9 THEN v_dist := v_d09;
    ELSE v_dist := v_d10;
    END IF;
    INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, ol_quantity, ol_amount,
                            ol_dist_info)
      VALUES (p_o_id, p_d_id, p_w_id, v_number, v_i_id, v_supply_w_id, v_quantity, v_amount, v_dist);
  END LOOP;
  INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local)
    VALUES (p_o_id, p_d_id, p_w_id, p_c_id, p_tstamp, p_ol_cnt, v_all_local);
  INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES (p_o_id, p_d_id, p_w_id);
END $$;

-- Payment (clause 2.5.2). The customer is p_c_id, or with p_byname = 1 the
-- one at position ceil(n / 2) by c_first of those named p_c_last (clause
-- 2.5.2.2). p_h_id is the history row's key, which the driver draws.
CREATE PROCEDURE payment(
  p_w_id INTEGER, p_d_id INTEGER, p_c_w_id INTEGER, p_c_d_id INTEGER, p_byname INTEGER, p_h_amount NUMERIC(6, 2),
  INOUT p_c_id INTEGER, INOUT p_c_last VARCHAR(16), INOUT p_c_credit VARCHAR(2), INOUT p_c_balance NUMERIC(12, 2),
  p_h_id BIGINT, p_tstamp TIMESTAMP)
LANGUAGE plpgsql AS $$
DECLARE
  v_w_name VARCHAR(10);
  v_d_name VARCHAR(10);
  v_count BIGINT;
  v_k BIGINT := 0;
  v_id INTEGER;
  v_last VARCHAR(16);
  v_data VARCHAR(500);
  v_h_data VARCHAR(24);
BEGIN
  UPDATE warehouse SET w_ytd = w_ytd + p_h_amount WHERE w_id = p_w_id;
  SELECT w_name INTO v_w_name FROM warehouse WHERE w_id = p_w_id;
  UPDATE district SET d_ytd = d_ytd + p_h_amount WHERE d_w_id = p_w_id AND d_id = p_d_id;
  SELECT d_name INTO v_d_name FROM district WHERE d_w_id = p_w_id AND d_id = p_d_id;
  IF p_byname = 1 THEN
    SELECT count(c_id) INTO v_count FROM customer WHERE c_last = p_c_last AND c_d_id = p_c_d_id AND c_w_id = p_c_w_id;
    FOR v_id, p_c_credit, p_c_balance IN
        SELECT c_id, c_credit, c_balance FROM customer
          WHERE c_w_id = p_c_w_id AND c_d_id = p_c_d_id AND c_last = p_c_last ORDER BY c_first LOOP
      v_k := v_k + 1;
      IF v_k = (v_count + 1) / 2 THEN
        EXIT;
      END IF;
    END LOOP;
    p_c_id := v_id;
  ELSE
    SELECT c_last, c_credit, c_balance INTO p_c_last, p_c_credit, p_c_balance
      FROM customer WHERE c_w_id = p_c_w_id AND c_d_id = p_c_d_id AND c_id = p_c_id;
  END IF;
  p_c_balance := p_c_balance - p_h_amount;
  IF p_c_credit = 'BC' THEN
    SELECT c_data INTO v_data FROM customer WHERE c_w_id = p_c_w_id AND c_d_id = p_c_d_id AND c_id = p_c_id;
    v_data := substr(p_c_id || ' ' || p_c_d_id || ' ' || p_c_w_id || ' ' || p_d_id || ' ' || p_w_id || ' ' ||
                     p_h_amount || ' | ' || v_data, 1, 500);
    UPDATE customer SET c_balance = p_c_balance, c_data = v_data
      WHERE c_w_id = p_c_w_id AND c_d_id = p_c_d_id AND c_id = p_c_id;
  ELSE
    UPDATE customer SET c_balance = p_c_balance WHERE c_w_id = p_c_w_id AND c_d_id = p_c_d_id AND c_id = p_c_id;
  END IF;
  v_h_data := v_w_name || '    ' || v_d_name;
  INSERT INTO history (h_id, h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id, h_date, h_amount, h_data)
    VALUES (p_h_id, p_c_d_id, p_c_w_id, p_c_id, p_d_id, p_w_id, p_tstamp, p_h_amount, v_h_data);
END $$;

-- Order-Status (clause 2.6.2): reads only. The customer as Payment picks
-- one, then their last order and its lines (p_lines, the count).
CREATE PROCEDURE ostat(
  p_w_id INTEGER, p_d_id INTEGER, INOUT p_c_id INTEGER, p_byname INTEGER, INOUT p_c_last VARCHAR(16),
  INOUT p_c_balance NUMERIC(12, 2), INOUT p_o_id INTEGER, INOUT p_entdate TIMESTAMP, INOUT p_carrier_id SMALLINT,
  INOUT p_lines INTEGER)
LANGUAGE plpgsql AS $$
DECLARE
  v_count BIGINT;
  v_k BIGINT := 0;
  v_first VARCHAR(16);
  v_middle VARCHAR(2);
  v_id INTEGER;
  v_i_id INTEGER;
  v_supply_w_id INTEGER;
  v_quantity SMALLINT;
  v_amount NUMERIC(6, 2);
  v_delivery_d TIMESTAMP;
BEGIN
  IF p_byname = 1 THEN
    SELECT count(c_id) INTO v_count FROM customer WHERE c_last = p_c_last AND c_d_id = p_d_id AND c_w_id = p_w_id;
    FOR p_c_balance, v_first, v_middle, v_id IN
        SELECT c_balance, c_first, c_middle, c_id FROM customer
          WHERE c_last = p_c_last AND c_d_id = p_d_id AND c_w_id = p_w_id ORDER BY c_first LOOP
      v_k := v_k + 1;
      IF v_k = (v_count + 1) / 2 THEN
        EXIT;
      END IF;
    END LOOP;
    p_c_id := v_id;
  ELSE
    SELECT c_balance, c_first, c_middle, c_last INTO p_c_balance, v_first, v_middle, p_c_last
      FROM customer WHERE c_id = p_c_id AND c_d_id = p_d_id AND c_w_id = p_w_id;
  END IF;
  SELECT o_id, o_carrier_id, o_entry_d INTO p_o_id, p_carrier_id, p_entdate
    FROM orders WHERE o_d_id = p_d_id AND o_w_id = p_w_id AND o_c_id = p_c_id ORDER BY o_id DESC LIMIT 1;
  p_lines := 0;
  IF FOUND THEN
    FOR v_i_id, v_supply_w_id, v_quantity, v_amount, v_delivery_d IN
        SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line
          WHERE ol_o_id = p_o_id AND ol_d_id = p_d_id AND ol_w_id = p_w_id LOOP
      p_lines := p_lines + 1;
    END LOOP;
  END IF;
END $$;

-- Delivery (clause 2.7.4): each district's oldest undelivered order, in one
-- transaction.
CREATE PROCEDURE delivery(p_w_id INTEGER, p_carrier_id INTEGER, p_tstamp TIMESTAMP)
LANGUAGE plpgsql AS $$
DECLARE
  v_o_id INTEGER;
  v_c_id INTEGER;
  v_total NUMERIC(12, 2);
BEGIN
  FOR v_d IN 1..10 LOOP
    SELECT no_o_id INTO v_o_id FROM new_order WHERE no_w_id = p_w_id AND no_d_id = v_d ORDER BY no_o_id LIMIT 1;
    IF FOUND THEN
      DELETE FROM new_order WHERE no_w_id = p_w_id AND no_d_id = v_d AND no_o_id = v_o_id;
      SELECT o_c_id INTO v_c_id FROM orders WHERE o_id = v_o_id AND o_d_id = v_d AND o_w_id = p_w_id;
      UPDATE orders SET o_carrier_id = p_carrier_id WHERE o_id = v_o_id AND o_d_id = v_d AND o_w_id = p_w_id;
      UPDATE order_line SET ol_delivery_d = p_tstamp WHERE ol_o_id = v_o_id AND ol_d_id = v_d AND ol_w_id = p_w_id;
      SELECT SUM(ol_amount) INTO v_total FROM order_line WHERE ol_o_id = v_o_id AND ol_d_id = v_d AND ol_w_id = p_w_id;
      UPDATE customer SET c_balance = c_balance + v_total WHERE c_id = v_c_id AND c_d_id = v_d AND c_w_id = p_w_id;
    END IF;
  END LOOP;
END $$;

-- Stock-Level (clause 2.8.2): reads only. The distinct items, among the
-- district's last 20 orders' lines, whose stock is below the threshold.
CREATE PROCEDURE slev(p_w_id INTEGER, p_d_id INTEGER, p_threshold INTEGER, INOUT p_stock_count BIGINT)
LANGUAGE plpgsql AS $$
DECLARE
  v_next_o_id INTEGER;
BEGIN
  SELECT d_next_o_id INTO v_next_o_id FROM district WHERE d_w_id = p_w_id AND d_id = p_d_id;
  SELECT COUNT(DISTINCT s_i_id) INTO p_stock_count FROM order_line, stock
    WHERE ol_w_id = p_w_id AND ol_d_id = p_d_id AND ol_o_id < v_next_o_id AND ol_o_id >= v_next_o_id - 20
      AND s_w_id = p_w_id AND s_i_id = ol_i_id AND s_quantity < p_threshold;
END $$;
