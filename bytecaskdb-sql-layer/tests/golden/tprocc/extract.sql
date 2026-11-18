SELECT o_id, EXTRACT(YEAR FROM o_entry_d), EXTRACT(SECOND FROM o_entry_d), EXTRACT(ISODOW FROM o_entry_d) FROM orders WHERE o_w_id = $1 AND o_d_id = $2 AND o_id = $3
