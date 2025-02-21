/* postgres can not */
USE plato;

SELECT
    i1.subkey AS sk,
    WeakField(i1.value1, "String", "funny") AS i1v1,
    WeakField(i1.value2, "String", "bunny") AS i1v2,
    WeakField(i2.value1, "String", "short") AS i2v1,
    WeakField(i2.value2, "String", "circuit") AS i2v2
FROM
    Input1 AS i1
JOIN
    Input2 AS i2
USING (subkey)
ORDER BY
    sk
;
