// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// tpcc_schema.cppm — the TPROC-C schema HammerDB builds, as the SQL DDL the
// catalog runs: the fixture the validation phase's hand-written plans are
// written against (#487, *Order*).
//
// Keys and secondary indexes are HammerDB's PostgreSQL schema's: the nine
// primary keys, and the unique customer_i2 and orders_i2. Column types are
// its MariaDB schema's, in the stage-1 types (docs/sql_layer_types_design.md):
// INT and TINYINT become INTEGER and SMALLINT where PostgreSQL's NUMERIC
// without a precision would not fit 64 bits, DATETIME becomes TIMESTAMP, and
// CHAR(n) becomes VARCHAR(n), stage 1 having no blank-padded comparison.
// HISTORY has no key in HammerDB; a table needs one here, so it gains h_id,
// supplied by the client until sequences exist (#491).

module;
#include <array>
#include <string_view>

export module bytecask.sql.tests.tpcc;

export namespace sql_test {

inline constexpr std::array<std::string_view, 11> kTpccSchema{
    "CREATE TABLE warehouse (w_id INTEGER NOT NULL, w_ytd NUMERIC(12, 2), w_tax NUMERIC(4, 4), "
    "w_name VARCHAR(10), w_street_1 VARCHAR(20), w_street_2 VARCHAR(20), w_city VARCHAR(20), "
    "w_state VARCHAR(2), w_zip VARCHAR(9), PRIMARY KEY (w_id))",

    "CREATE TABLE district (d_id SMALLINT NOT NULL, d_w_id INTEGER NOT NULL, d_ytd NUMERIC(12, 2), "
    "d_tax NUMERIC(4, 4), d_next_o_id INTEGER, d_name VARCHAR(10), d_street_1 VARCHAR(20), "
    "d_street_2 VARCHAR(20), d_city VARCHAR(20), d_state VARCHAR(2), d_zip VARCHAR(9), "
    "PRIMARY KEY (d_w_id, d_id))",

    "CREATE TABLE customer (c_id INTEGER NOT NULL, c_d_id SMALLINT NOT NULL, c_w_id INTEGER NOT NULL, "
    "c_first VARCHAR(16), c_middle VARCHAR(2), c_last VARCHAR(16), c_street_1 VARCHAR(20), "
    "c_street_2 VARCHAR(20), c_city VARCHAR(20), c_state VARCHAR(2), c_zip VARCHAR(9), c_phone VARCHAR(16), "
    "c_since TIMESTAMP, c_credit VARCHAR(2), c_credit_lim NUMERIC(12, 2), c_discount NUMERIC(4, 4), "
    "c_balance NUMERIC(12, 2), c_ytd_payment NUMERIC(12, 2), c_payment_cnt INTEGER, c_delivery_cnt INTEGER, "
    "c_data VARCHAR(500), PRIMARY KEY (c_w_id, c_d_id, c_id))",

    "CREATE UNIQUE INDEX customer_i2 ON customer (c_w_id, c_d_id, c_last, c_first, c_id)",

    "CREATE TABLE history (h_id BIGINT NOT NULL, h_c_id INTEGER, h_c_d_id SMALLINT, h_c_w_id INTEGER, "
    "h_d_id SMALLINT, h_w_id INTEGER, h_date TIMESTAMP, h_amount NUMERIC(6, 2), h_data VARCHAR(24), "
    "PRIMARY KEY (h_id))",

    "CREATE TABLE new_order (no_w_id INTEGER NOT NULL, no_d_id SMALLINT NOT NULL, no_o_id INTEGER NOT NULL, "
    "PRIMARY KEY (no_w_id, no_d_id, no_o_id))",

    "CREATE TABLE orders (o_id INTEGER NOT NULL, o_w_id INTEGER NOT NULL, o_d_id SMALLINT NOT NULL, "
    "o_c_id INTEGER, o_carrier_id SMALLINT, o_ol_cnt SMALLINT, o_all_local SMALLINT, o_entry_d TIMESTAMP, "
    "PRIMARY KEY (o_w_id, o_d_id, o_id))",

    "CREATE UNIQUE INDEX orders_i2 ON orders (o_w_id, o_d_id, o_c_id, o_id)",

    "CREATE TABLE order_line (ol_w_id INTEGER NOT NULL, ol_d_id SMALLINT NOT NULL, ol_o_id INTEGER NOT NULL, "
    "ol_number SMALLINT NOT NULL, ol_i_id INTEGER, ol_delivery_d TIMESTAMP, ol_amount NUMERIC(6, 2), "
    "ol_supply_w_id INTEGER, ol_quantity SMALLINT, ol_dist_info VARCHAR(24), "
    "PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number))",

    "CREATE TABLE item (i_id INTEGER NOT NULL, i_im_id INTEGER, i_name VARCHAR(24), i_price NUMERIC(5, 2), "
    "i_data VARCHAR(50), PRIMARY KEY (i_id))",

    "CREATE TABLE stock (s_i_id INTEGER NOT NULL, s_w_id INTEGER NOT NULL, s_quantity SMALLINT, "
    "s_dist_01 VARCHAR(24), s_dist_02 VARCHAR(24), s_dist_03 VARCHAR(24), s_dist_04 VARCHAR(24), "
    "s_dist_05 VARCHAR(24), s_dist_06 VARCHAR(24), s_dist_07 VARCHAR(24), s_dist_08 VARCHAR(24), "
    "s_dist_09 VARCHAR(24), s_dist_10 VARCHAR(24), s_ytd INTEGER, s_order_cnt SMALLINT, s_remote_cnt SMALLINT, "
    "s_data VARCHAR(50), PRIMARY KEY (s_i_id, s_w_id))",
};

}  // namespace sql_test
