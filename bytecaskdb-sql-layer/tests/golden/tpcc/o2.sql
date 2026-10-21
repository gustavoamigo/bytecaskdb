SELECT c_balance, c_first, c_middle, c_id FROM customer WHERE c_last = $1 AND c_d_id = $2 AND c_w_id = $3 ORDER BY c_first
