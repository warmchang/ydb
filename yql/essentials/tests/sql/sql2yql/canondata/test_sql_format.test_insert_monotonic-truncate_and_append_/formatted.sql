USE plato;

INSERT INTO Output WITH TRUNCATE
SELECT
    *
FROM
    Input1
ORDER BY
    key,
    subkey
;

INSERT INTO Output WITH MONOTONIC_KEYS
SELECT
    *
FROM
    Input2
ORDER BY
    key,
    subkey
;
