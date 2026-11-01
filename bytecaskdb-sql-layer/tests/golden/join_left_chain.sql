SELECT t.id, p.v, r.s FROM t LEFT JOIN p ON p.a = t.qty AND p.b = $1 LEFT JOIN r ON r.id = p.w WHERE t.name = $2
