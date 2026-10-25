SELECT p.v FROM r, p WHERE r.id = $1 AND p.a = $2 AND p.v = r.s ORDER BY p.b DESC NULLS LAST
