SELECT t.id, p.b FROM t, p WHERE t.name = $1 AND p.b = t.qty AND p.v = t.qty
