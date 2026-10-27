SELECT t.id, p.v, r.s FROM t, p, r WHERE t.name = $1 AND p.a = t.qty AND p.b = $2 AND r.id = p.w
