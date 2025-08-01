# PostgreSQL UDF

<!-- markdownlint-disable blanks-around-fences -->

YQL предоставляет возможность доступа к [функциям](https://www.postgresql.org/docs/16/functions.html) и [типам данных](https://www.postgresql.org/docs/16/datatype.html) PostgreSQL.

Имена PostgreSQL типов в YQL получаются добавлением префикса `Pg` к исходному имени типа.
Например `PgVarchar`, `PgInt4`, `PgText`. Имена pg типов (как и вообще всех типов) в YQL являются case-insensitive. На данный момент поддерживаются все простые типы данных из PostgreSQL, а также массивы.

Если исходный тип является типом массива (в PostgreSQL такие типы начинаются с подчеркивания: `_int4` - массив 32-битных целых), то имя типа в YQL тоже начинается с подчеркивания – `_PgInt4`.

## Литералы {#literals}

Строковые и числовые литералы Pg типов можно создавать с помощью специальных суффиксов (аналогично простым [строковым](../../syntax/lexer.md#string-literals) и [числовым](../../syntax/lexer.md#literal-numbers) литералам).

### Целочисленные литералы {#intliterals}

Суффикс | Тип | Комментарий
----- | ----- | -----
`p` | `PgInt4` | 32-битное знаковое целое (в PostgreSQL нет беззнаковых типов)
`ps`| `PgInt2` | 16-битное знаковое целое
`pi`| `PgInt4` |
`pb`| `PgInt8` | 64-битное знаковое цело
`pn`| `PgNumeric` | знаковое целое произвольной точности (до 131072 цифр)

### Литералы с плавающей точкой {#floatliterals}

Суффикс | Тип | Комментарий
----- | ----- | -----
`p` | `PgFloat8` | число с плавающей точкой (64 бит double)
`pf4`| `PgFloat4` | число с плавающей точкой (32 бит float)
`pf8`| `PgFloat8` |
`pn` | `PgNumeric` | число с плавающей точкой произвольной точности (до 131072 цифр перед запятой, до 16383 цифр после запятой)

### Строковые литералы {#stringliterals}

Суффикс | Тип | Комментарий
----- | ----- | -----
`p` | `PgText` | текстовая строка
`pt`| `PgText` |
`pv`| `PgVarchar` | текстовая строка
`pb`| `PgBytea` | бинарная строка

{% note warning "Внимание" %}

Значения строковых/числовых литералов (т.е. то что идет перед суффиксом) должны быть валидной строкой/числом с точки зрения YQL.
В частности, должны соблюдаться правила эскейпинга YQL, а не PostgreSQL.

{% endnote %}

Пример:

```yql
SELECT
    1234p,       -- pgint4
    0x123pb,     -- pgint8
    "тест"pt,    -- pgtext
    123e-1000pn; -- pgnumeric
;
```

### Литерал массива {#array-literal}

Для построения литерала массива используется функция `PgArray`:

```yql
SELECT
    PgArray(1p, NULL ,2p) -- {1,NULL,2}, тип _int4
;
```

### Конструктор литералов произвольного типа {#literals_constructor}

Литералы всех типов (в том числе и Pg типов) могут создаваться с помощью конструктора литералов со следующей сигнатурой:
`Имя_типа(<строковая константа>)`.

Напрмер:

```yql
DECLARE $foo AS String;
SELECT
    PgInt4("1234"), -- то же что и 1234p
    PgInt4(1234),   -- в качестве аргумента можно использовать литеральные константы
    PgInt4($foo),   -- и declare параметры
    PgBool(true),
    PgInt8(1234),
    PgDate("1932-01-07"),
;
```

Также поддерживается встроенная функция `PgConst` со следующей сигнатурой: `PgConst(<строковое значение>, <тип>)`.
Такой способ более удобен для кодогенерации.

Например:

```yql
SELECT
    PgConst("1234", PgInt4), -- то же что и 1234p
    PgConst("true", PgBool)
;
```

Для некоторых типов в функции `PgConst` можно указать дополнительные модификаторы. Возможные модификаторы для типа `pginterval` перечислены в [документации PostgreSQL](https://www.postgresql.org/docs/16/datatype-datetime.html).

```yql
SELECT
    PgConst(90, pginterval, "day"), -- 90 days
    PgConst(13.45, pgnumeric, 10, 1); -- 13.5
;
```


## Операторы {#operators}

Операторы PostgreSQL (унарные и бинарные) доступны через встроенную функцию `PgOp(<оператор>, <операнды>)`:

```yql
SELECT
    PgOp("*", 123456789987654321pn, 99999999999999999999pn), --  12345678998765432099876543210012345679
    PgOp('|/', 10000.0p), -- 100.0p (квадратный корень)
    PgOp("-", 1p), -- -1p
    -1p,           -- унарный минус для литералов работает и без PgOp
;
```

## Оператор приведения типа {#cast_operator}

Для приведения значения одного Pg типа к другому используется встроенная функция `PgCast(<исходное значение>, <желаемый тип>)`:

```yql
SELECT
    PgCast(123p, PgText), -- преобразуем число в строку
;
```

При преобразовании из строковых Pg типов в некоторые целевые типы можно указать дополнительные модификаторы. Возможные модификаторы для типа `pginterval` перечислены в [документации](https://www.postgresql.org/docs/16/datatype-datetime.html).

```yql
SELECT
    PgCast('90'p, pginterval, "day"), -- 90 days
    PgCast('13.45'p, pgnumeric, 10, 1); -- 13.5
;
```

## Преобразование значений Pg типов в значения YQL типов и обратно {#frompgtopg}

Для некоторых Pg типов возможна конвертация в YQL типы и обратно. Конвертация осуществляется с помощью встроенных функций
`FromPg(<значение Pg типа>)` и `ToPg(<значение YQL типа>)`:

```yql
SELECT
    FromPg("тест"pt), -- Just(Utf8("тест")) - pg типы всегда nullable
    ToPg(123.45), -- 123.45pf8
;
```

### Список псевдонимов типов PostgreSQL при их использовании в YQL {#pgyqltypes}

Ниже приведены типы данных YQL, соответствующие им логические типы PostgreSQL и названия типов PostgreSQL при их использовании в YQL:

| YQL | PostgreSQL | Название PostgreSQL-типа в YQL|
|---|---|---|
| `Bool` | `bool` |`pgbool` |
| `Int8` | `int2` |`pgint2` |
| `Uint8` | `int2` |`pgint2` |
| `Int16` | `int2` |`pgint2` |
| `Uint16` | `int4` |`pgint4` |
| `Int32` | `int4` |`pgint4` |
| `Uint32` | `int8` |`pgint8` |
| `Int64` | `int8` |`pgint8` |
| `Uint64` | `numeric` |`pgnumeric` |
| `Float` | `float4` |`pgfloat4` |
| `Double` | `float8` |`pgfloat8` |
| `String` | `bytea` |`pgbytea` |
| `Utf8` | `text` |`pgtext` |
| `Yson` | `bytea` |`pgbytea` |
| `Json` | `json` |`pgjson` |
| `Uuid` | `uuid` |`pguuid` |
| `JsonDocument` | `jsonb` |`pgjsonb` |
| `Date` | `date` |`pgdate` |
| `Datetime` | `timestamp` |`pgtimestamp` |
| `Timestamp` | `timestamp` |`pgtimestamp` |
| `Interval` | `interval` | `pginterval` |
| `TzDate` | `text` |`pgtext` |
| `TzDatetime` | `text` |`pgtext` |
| `TzTimestamp` | `text` |`pgtext` |
| `Date32` | `date` | `pgdate`|
| `Datetime64` | `timestamp` |`pgtimestamp` |
| `Timestamp64` | `timestamp` |`pgtimestamp` |
| `Interval64`| `interval` |`pginterval` |
| `TzDate32` | `text` |  |`pgtext` |
| `TzDatetime64` | `text` |  |`pgtext` |
| `TzTimestamp64` | `text` |  |`pgtext` |
| `Decimal` | `numeric` |`pgnumeric` |
| `DyNumber` | `numeric` |`pgnumeric` |


### Таблица соответствия типов `ToPg` {#topg}

Таблица соответствия типов данных YQL и PostgreSQL при использовании функции `ToPg`:

{% include [topg](../../_includes/topg.md) %}

### Таблица соответствия типов `FromPg` {#frompg}

Таблица соответствия типов данных PostgreSQL и YQL при использовании функции `FromPg`:

{% include [frompg](../../_includes/frompg.md) %}

## Вызов PostgreSQL функций {#callpgfunction}

Чтобы вызвать PostgreSQL функцию, необходимо добавить префикс `Pg::` к ее имени:

```yql
SELECT
    Pg::extract('isodow'p,PgCast('1961-04-12'p,PgDate)), -- 3pn (среда) - работа с датами до 1970 года
    Pg::generate_series(1p,5p), -- [1p,2p,3p,4p,5p] - для функций-генераторов возвращается ленивый список
;
```

Существует также альтернативный способ вызова функций через встроенную функцию `PgCall(<имя функции>, <операнды>)`:

```yql
SELECT
    PgCall('lower', 'Test'p), -- 'test'p
;
```

При вызове функции, возвращающей набор `pgrecord`, можно распаковать результат в список структур, используя функцию `PgRangeCall(<имя функции>, <операнды>)`:

```yql
SELECT * FROM
    AS_TABLE(PgRangeCall("json_each", pgjson('{"a":"foo", "b":"bar"}')));
    --- 'a'p,pgjson('"foo"')
    --- 'b'p,pgjson('"bar"')
;
```

## Вызов агрегационных PostgreSQL функций {#pgaggrfunction}

Чтобы вызвать агрегационную PostgreSQL функцию, необходимо добавить префикс `Pg::` к ее имени:

```yql
SELECT
Pg::string_agg(x,','p)
FROM (VALUES ('a'p),('b'p),('c'p)) as a(x); -- 'a,b,c'p

SELECT
Pg::string_agg(x,','p) OVER (ORDER BY x)
FROM (VALUES ('a'p),('b'p),('c'p)) as a(x); -- 'a'p,'a,b'p,'a,b,c'p
;
```

Также можно использовать агрегационную PostgreSQL функцию для построения фабрики агрегационных функций с последующим применением в `AGGREGATE_BY`:

```yql
$agg_max = AggregationFactory("Pg::max");

SELECT
AGGREGATE_BY(x,$agg_max)
FROM (VALUES ('a'p),('b'p),('c'p)) as a(x); -- 'c'p

SELECT
AGGREGATE_BY(x,$agg_max) OVER (ORDER BY x),
FROM (VALUES ('a'p),('b'p),('c'p)) as a(x); -- 'a'p,'b'p,'c'p
```

В этом случае вызов `AggregationFactory` принимает только имя функции с префиксом `Pg::`, а все аргументы функции передаются в `AGGREGATE_BY`.

Если в агрегационной функции не один аргумент, а ноль или два и более, необходимо использовать кортеж при вызове `AGGREGATE_BY`:

```yql
$agg_string_agg = AggregationFactory("Pg::string_agg");

SELECT
AGGREGATE_BY((x,','p),$agg_string_agg)
FROM (VALUES ('a'p),('b'p),('c'p)) as a(x); -- 'a,b,c'p

SELECT
AGGREGATE_BY((x,','p),$agg_string_agg) OVER (ORDER BY x)
FROM (VALUES ('a'p),('b'p),('c'p)) as a(x); -- 'a'p,'a,b'p,'a,b,c'p
```

{% note warning "Внимание" %}

Не поддерживается режим `DISTINCT` над аргументами при вызове агрегационных PostgreSQL функций, а также использование `MULTI_AGGREGATE_BY`.

{% endnote %}

## Логические операции {#logic-operations}

Для выполнения логических операций используются функции `PgAnd`, `PgOr`, `PgNot`:

```yql
SELECT
    PgAnd(PgBool(true), PgBool(true)), -- PgBool(true)
    PgOr(PgBool(false), null), -- PgCast(null, pgbool)
    PgNot(PgBool(true)), -- PgBool(false)
;
```
