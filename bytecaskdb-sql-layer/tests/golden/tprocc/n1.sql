SELECT c_discount, c_last, c_credit, w_tax FROM customer, warehouse WHERE w_id = $1 AND c_w_id = $1 AND c_d_id = $2 AND c_id = $3
