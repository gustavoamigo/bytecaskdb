SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line WHERE ol_o_id = $1 AND ol_d_id = $2 AND ol_w_id = $3
