SELECT t.id, p.v FROM t, p WHERE t.name = $1 AND p.w = t.qty ORDER BY p.v DESC LIMIT 10
