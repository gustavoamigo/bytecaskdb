SELECT t.qty, count(*), sum(p.v) FROM t, p WHERE p.w = t.qty GROUP BY t.qty
