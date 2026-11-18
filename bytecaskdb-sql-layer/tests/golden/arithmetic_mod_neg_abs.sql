SELECT id, qty % 3, -qty, abs(qty) FROM t WHERE id = $1
