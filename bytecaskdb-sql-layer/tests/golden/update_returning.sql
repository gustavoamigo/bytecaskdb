UPDATE t SET qty = $1 WHERE id = $2 RETURNING id, qty + 1
