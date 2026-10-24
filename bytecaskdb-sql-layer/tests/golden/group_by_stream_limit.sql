SELECT a, b, max(w) FROM p WHERE a = $1 GROUP BY a, b LIMIT 2
