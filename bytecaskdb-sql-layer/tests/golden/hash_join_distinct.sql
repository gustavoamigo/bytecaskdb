SELECT count(DISTINCT p.v) FROM t, p WHERE t.name > $1 AND p.w = t.qty
