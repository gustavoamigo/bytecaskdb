SELECT d_next_o_id, d_tax FROM district WHERE d_id = $1 AND d_w_id = $2 FOR UPDATE
