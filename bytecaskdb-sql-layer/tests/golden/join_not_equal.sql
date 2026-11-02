SELECT t.id, p.v FROM t JOIN p ON t.qty <> p.w WHERE t.name = $1
