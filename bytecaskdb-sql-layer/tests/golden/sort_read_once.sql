SELECT p.v FROM r, p WHERE r.id = $1 AND p.a = $2 AND p.w = r.s ORDER BY p.v NULLS FIRST LIMIT 5
