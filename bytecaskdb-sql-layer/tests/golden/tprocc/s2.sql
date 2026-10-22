SELECT COUNT(DISTINCT s_i_id) FROM order_line, stock WHERE ol_w_id = $1 AND ol_d_id = $2 AND ol_o_id < $3 AND ol_o_id >= $3 - 20 AND s_w_id = $1 AND s_i_id = ol_i_id AND s_quantity < $4
