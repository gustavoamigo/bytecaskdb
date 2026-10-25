SELECT t.id, p.a FROM t, p WHERE t.name = $1 AND p.v = t.qty ORDER BY p.b LIMIT 10
