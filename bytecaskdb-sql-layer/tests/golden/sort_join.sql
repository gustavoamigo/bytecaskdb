SELECT p.v, r.s FROM p, r WHERE p.a = $1 AND r.id = p.w ORDER BY r.s, p.v DESC
