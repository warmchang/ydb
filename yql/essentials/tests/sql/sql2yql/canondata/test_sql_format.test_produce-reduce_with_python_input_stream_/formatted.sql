/* custom error: Cannot execute ScriptUdf over stream/flow inside DQ stage */
USE plato;

$udfScript = @@
import functools
def Len(key, input):
    return {"value":functools.reduce(lambda x,y: x + 1, input, 0)}
@@;

$udf = Python::Len(Callable<(String, Stream<String>) -> Struct<value: Uint32>>, $udfScript);

$res = (
    REDUCE Input1
    ON
        key
    USING $udf(value)
);

SELECT
    *
FROM
    $res
ORDER BY
    value
;
