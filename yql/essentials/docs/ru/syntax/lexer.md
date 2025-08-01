

# Лексическая структура

<!-- markdownlint-disable blanks-around-fences -->

Запрос на языке YQL представляет собой валидный UTF-8 текст, который состоит из *команд* (statement), разделенных символом точка с запятой (`;`). Последняя точка с запятой при этом может отсутствовать.

Каждая команда, в свою очередь, состоит из последовательности *токенов*, допустимых для данной команды. Токеном может быть *ключевое слово*, *идентификатор*, *литерал* и другие. Токены разделяются пробельными символами (пробел, табуляция, перевод строки) либо *комментариями*. Комментарий не является частью команды и синтаксически эквивалентен пробельному символу.

## Режимы совместимости синтаксиса {#lexer-modes}

Поддерживаются два режима совместимости синтаксиса:

* Расширенный C++ (по умолчанию)
* ANSI SQL

Режим ANSI SQL включается с помощью специального комментария `--!ansi_lexer`, который должен стоять в начале запроса.

Особенности интерпретации лексических элементов в разных режимах совместимости описаны ниже.

## Комментарии {#comments}

Поддерживаются следующие виды комментариев:

* Однострочные: начинается с последовательности символов `--` (два минуса *подряд*) и продолжается до конца строки
* Многострочные: начинается с последовательности символов `/*` и заканчивается на последовательности символов `*/`

```yql
SELECT 1; -- A single-line comment
/*
   Some multi-line comment
*/
```

В режиме совместимости синтаксиса C++ (по умолчанию) многострочный комментарий заканчивается на *ближайшей* последовательности символов `*/`.

В режиме совместимости синтаксиса ANSI SQL учитывается вложенность многострочных комментариев:

```yql
--!ansi_lexer
SELECT * FROM T; /* комментарий /* вложенный комментарий, без ansi_lexer будет ошибка  */ */
```

## Ключевые слова и идентификаторы {#keywords-and-ids}

**Ключевые слова** – это токены, имеющие фиксированное значение в языке YQL. Примеры ключевых слов – `SELECT`, `INSERT`, `FROM`, `ACTION` и т. д. Ключевые слова регистронезависимы, то есть `SELECT` и `SeLEcT` эквивалентны.

Список ключевых слов не фиксирован – по мере развития языка он будет расширяться. Ключевое слово не может содержать цифры и начинаться или заканчиваться символом подчеркивания.

**Идентификаторы** – это токены, которые идентифицируют имена таблиц, колонок и других объектов в YQL. Идентификаторы в YQL всегда регистрозависимы.

Идентификатор может быть записан в теле программы без специального оформления, если он:

* не является ключевым словом;
* начинается с латинской буквы или подчеркивания;
* последующими символами могут быть латинская буква, подчеркивание или цифра.

```yql
SELECT my_column FROM my_table; -- my_column and my_table are identifiers
```

Для записи в теле запроса произвольного идентификатора он заключается в обратные кавычки (бэктики):

```yql
SELECT `column with space` from T;
SELECT * FROM `my_dir/my_table`;
```

Идентификатор в обратных кавычках никогда не интерпретируется как ключевое слово:

```yql
SELECT `select` FROM T; -- select - имя колонки в таблице T
```

При использовании обратных кавычек применим стандартный C-эскейпинг:

```yql
SELECT 1 as `column with\n newline, \x0a newline and \` backtick `;
```

В режиме совместимости синтаксиса ANSI SQL произвольные идентификаторы также могут быть выделены заключением их в двойные кавычки. Для включения двойной кавычки в идентификатор в кавычках она должна быть удвоена:

```yql
--!ansi_lexer
SELECT 1 as "column with "" double quoute"; -- имя колонки будет: column with " double quoute
```

## SQL хинты {#sql-hints}

SQL хинты – это специальные настройки, которые позволяют пользователю влиять на план выполнения запроса (например, включать/выключать определенные оптимизации, форсировать стратегию JOIN-а и т. п.). В отличие от [PRAGMA](pragma/index.md), SQL хинты обладают локальным действием – они привязаны к определенной точке YQL запроса (обычно следуют после ключевого слова) и влияют только на соответствующий statement или даже его часть.

SQL хинты представляют собой набор настроек "имя-список значений" и задаются внутри комментариев специального вида –
первым символом комментария с SQL хинтами должен быть `+`:

```yql
--+ Name1(Value1 Value2 Value3) Name2(Value4) ...
```

Имя SQL хинта должно состоять из алфавитно-цифровых ASCII символов и начинаться с буквы. Регистр букв в имени хинта игнорируется. После имени хинта в скобках задается произвольное количество значений, разделенных пробелами. В качестве значения может выступать произвольный набор символов.

Если в наборе символов значения имеется пробел или скобка, то необходимо использовать одинарные кавычки:

```yql
--+ foo('(value with space and paren)')
```

```yql
--+ foo('value1' value2)
-- эквивалетно
--+ foo(value1 value2)
```

Одинарную кавычку внутри значения необходимо эскейпить путем дублирования:

```yql
--+ foo('value with single quote '' inside')
```

Если в наборе задано два и более хинтов с одинаковыми именами, используется последний из них:
```sql
--+ foo(v1 v2) bar(v3) foo()
-- эквивалетно
--+ bar(v3) foo()
```

Неизвестные имена SQL хинтов (либо синтаксически некорректные хинты) никогда не вызывают ошибок – они просто игнорируются:

```yql
--+ foo(value1) bar(value2  baz(value3)
-- из-за пропущенной закрывающей скобки в bar эквивалетно
--+ foo(value1)
```

Такое поведение связано с нежеланием ломать написанные ранее валидные YQL запросы с комментариями, которые похожи на хинты. При этом синтаксически корректные SQL хинты в неожиданном для YQL месте вызывают предупреждение:

```yql
-- в данный момент хинты после SELECT не поддерживаются
SELECT /*+ foo(123) */ 1; -- предупреждение 'Hint foo will not be used'
```

{% note info %}

SQL хинты – это именно подсказки оптимизатору, поэтому:

* Хинты никогда не влияют на результат запроса.
* По мере развития оптимизаторов в YQL вполне возможна ситуация, в которой хинт становится неактуальным и начнет игнорироваться. Например, полностью поменялся алгоритм, который настраивался данным хинтом, либо оптимизатор настолько улучшился, что гарантированно выбирает оптимальное решение, поэтому какие-то ручные настройки будут скорее вредить.

{% endnote %}

## Строковые литералы {#string-literals}

Строковый литерал (константа) записывается как последовательность символов, заключенных в одинарные кавычки. Внутри строкового литерала можно использовать правила эскейпинга в стиле C:

```yql
SELECT 'string with\n newline, \x0a newline and \' backtick ';
```

В режиме совместимости синтаксиса С++ (по-умолчанию) разрешается использовать вместо одинарных кавычек двойные:

```yql
SELECT "string with\n newline, \x0a newline and \" backtick ";
```

В режиме совместимости синтаксиса ANSI SQL двойные кавычки используются для идентификаторов, а единственный вид эскепинга который действует для строковых литералов – это  дублирование символа одиночной кавычки:

```yql
--!ansi_lexer
SELECT 'string with '' quote'; -- результат: string with ' quote
```

На основании строковых литералов могут быть получены [литералы простых типов](../builtins/basic#data-type-literals).

### Многострочные строковые литералы {#multiline-string-literals}

Многострочный строковой литерал записывается в виде произвольного набора символов между двойными собачками `@@`:

```yql
$text = @@some
multiline
text@@;
SELECT LENGTH($text);
```

Если необходимо вставить в текст двойную собачку, ее необходимо удвоить:

```yql
$text = @@some
multiline with double at: @@@@
text@@;
SELECT $text;
```

### Типизированные строковые литералы {#typed-string-literals}

* Для строкового литерала, включая [многострочный](#multiline-string-literals), по умолчанию используется тип `String` (см. также [PRAGMA UnicodeLiterals](pragma/global.md#UnicodeLiterals)).
* С помощью следующих суффиксов можно явно управлять типом литерала:

  * `s` — `String`;
  * `u` — `Utf8`;
  * `y` — `Yson`;
  * `j` — `Json`.

#### Пример

```yql
SELECT "foo"u, '[1;2]'y, @@{"a":null}@@j;
```

## Числовые литералы {#literal-numbers}

* Целочисленные литералы по умолчанию имеют тип `Int32`, если попадают в его диапазон, и в противном случае автоматически расширяются до `Int64`.
* С помощью следующих суффиксов можно явно управлять типом литерала:

  * `l` — `Int64`;
  * `s` — `Int16`;
  * `t` — `Int8`.

* Добавление суффикса `u` превращает тип в соответствующий беззнаковый:

  * `ul` — `Uint64`;
  * `u`  — `Uint32`;
  * `us` — `Uint16`;
  * `ut` — `Uint8`.

* Также для целочисленных литералов доступна запись в шестнадцатеричной, восьмеричной и двоичной форме с помощью префиксов `0x`, `0o` и `0b`, соответственно. Их можно произвольным образом комбинировать с описанными выше суффиксами.
* Литералы с плавающей точкой по умолчанию имеют тип `Double`, но с помощью суффикса `f` его можно сузить до `Float`.

```yql
SELECT
  123l AS `Int64`,
  0b01u AS `Uint32`,
  0xfful AS `Uint64`,
  0o7ut AS `Uint8`,
  456s AS `Int16`,
  1.2345f AS `Float`;
```

## Литералы PostgreSQL {#pgliterals}

Строковые и числовые литералы Pg типов можно создавать с помощью специальных суффиксов (аналогично простым [строковым](#string-literals) и [числовым](#literal-numbers) литералам).

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

Значения строковых/числовых литералов (т. е. то, что идет перед суффиксом) должны быть валидной строкой/числом с точки зрения YQL. В частности, должны соблюдаться правила эскейпинга YQL, а не PostgreSQL.

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

Литералы всех типов (в том числе и Pg типов) могут создаваться с помощью конструктора литералов со следующей сигнатурой: `Имя_типа(<строковая константа>)`.

Например:

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

Также поддерживается встроенная функция `PgConst` со следующей сигнатурой: `PgConst(<строковое значение>, <тип>)`. Такой способ более удобен для кодогенерации. Например:

```yql
SELECT
    PgConst("1234", PgInt4), -- то же что и 1234p
    PgConst("true", PgBool)
;
```

Для некоторых типов в функции `PgConst` можно указать дополнительные модификаторы. Возможные модификаторы для типа `pginterval` перечислены в [документации PostgreSQL](https://www.postgresql.org/docs/16/datatype-datetime.html).

```yql
SELECT
    PgConst(90, pginterval, "day"),   -- 90 days
    PgConst(13.45, pgnumeric, 10, 1); -- 13.5
;
```
