# Системные представления базы данных

Для получения служебной информации о состоянии базы данных можно обращаться к системным представлениям (system views). Они  доступны из корня дерева базы данных и используют системный префикс пути `.sys`.

{% note info %}

Частое обращение к системным представлениям приводит к дополнительной нагрузке на базу данных, особенно в случае большого размера базы. Превышение частоты в 1 запрос в секунду не рекомендуется.

{% endnote %}

## Партиции {#partitions}

Следующее системное представление хранит детализированную информацию об [партициях](../concepts/datamodel/table.md#partitioning)  таблиц базы данных:

* `partition_stats` — содержит информацию о моментальных метриках и кумулятивных счетчиках операций. К первым относятся, например, данные о нагрузке на CPU или количестве выполняемых [транзакций](../concepts/transactions.md). Ко вторым — общее количество прочитанных строк.

Предназначено, например, для выявления неравномерно нагруженных партиций или отображения размера данных в них.

Моментальные метрики (`NodeId`, `AccessTime`, `CPUCores` и т.д.) содержат мгновенные значения.
Кумулятивные (не моментальные) метрики (`RowReads`, `RowUpdates`, `LocksAcquired` и т.д.) хранят накопленные значения с момента последнего старта таблетки (`StartTime`), обслуживающей партицию.

Структура представления:

| Колонка | Описание | Тип данных | Моментальная/кумулятивная |
| --- | --- | --- | --- |
| `OwnerId` | Идентификатор SchemeShard таблицы.<br/>Ключ: `0`. | `Uint64` | Моментальная |
| `PathId` | Идентификатор пути в SchemeShard.<br/>Ключ: `1`. | `Uint64` | Моментальная |
| `PartIdx` | Порядковый номер партиции.<br/>Ключ: `2`. | `Uint64` | Моментальная |
| `FollowerId` | Идентификатор [подписчика](../concepts/glossary.md#tablet-follower) таблетки партиции. Значение 0 означает лидера.<br/>Ключ: `3`. | `Uint32` | Моментальная |
| `DataSize` | Приблизительный размер данных партиции в байтах. | `Uint64` | Моментальная |
| `RowCount` | Приблизительное количество строк. | `Uint64` | Моментальная |
| `IndexSize` | Размер индекса партиции в байтах. | `Uint64` | Моментальная |
| `CPUCores` | Моментальное значение нагрузки на партицию (доля времени ядра процессора, затраченного актором партиции). | `Double` | Моментальная |
| `TabletId` | Идентификатор таблетки партиции. | `Uint64` | Моментальная |
| `Path` | Полный путь к таблице. | `Utf8` | Моментальная |
| `NodeId` | Идентификатор ноды, на которой в данный момент обслуживается партиция. | `Uint32` | Моментальная |
| `StartTime` | Последний момент запуска таблетки партиции. | `Timestamp` | Моментальная |
| `AccessTime` | Последний момент чтения из партиции. | `Timestamp` | Моментальная |
| `UpdateTime` | Последний момент записи в партицию. | `Timestamp` | Моментальная |
| `RowReads` | Количество чтений по ключу. | `Uint64` | Кумулятивная |
| `RowUpdates` | Количество записанных строк . | `Uint64` | Кумулятивная |
| `RowDeletes` | Количество удалённых строк. | `Uint64` | Кумулятивная |
| `RangeReads` | Количество чтений по диапазону ключей. | `Uint64` | Кумулятивная |
| `RangeReadRows` | Количество строк, прочитанных в диапазонах. | `Uint64` | Кумулятивная |
| `InFlightTxCount` | Количество исполняющихся транзакций. | `Uint64` | Моментальная |
| `ImmediateTxCompleted` | Количество завершившихся [одношардовых транзакций](../concepts/glossary.md#transactions). | `Uint32` | Кумулятивная |
| `CoordinatedTxCompleted` | Количество завершившихся [распределенных транзакций](../concepts/glossary.md#transactions). | `Uint64` | Кумулятивная |
| `TxRejectedByOverload` | Количество транзакций, отменённых по причине [высокой нагрузки](../troubleshooting/performance/queries/overloaded-errors.md). | `Uint64` | Кумулятивная |
| `TxRejectedByOutOfStorage` | Количество транзакций, отменённых из-за нехватки места в хранилище. | `Uint64` | Кумулятивная |
| `LastTtlRunTime` | Последний момент запуска очистки партиции по TTL | `Timestamp` | Моментальная |
| `LastTtlRowsProcessed` | Количество проверенных строк партиции при последней очистке по TTL | `Uint64` | Моментальная |
| `LastTtlRowsErased` | Количество удалённых строк партиции при последней очистке по TTL | `Uint64` | Моментальная |
| `LocksAcquired` | Количество установленных [блокировок](../contributor/datashard-locks-and-change-visibility.md) . | `Uint64` | Кумулятивная |
| `LocksWholeShard` | Количество установленных [блокировок "весь шард"](../contributor/datashard-locks-and-change-visibility.md#ограничения). | `Uint64` | Кумулятивная |
| `LocksBroken` | Количество [сломанных блокировок](../contributor/datashard-locks-and-change-visibility.md#высокоуровневая-схема-работы). | `Uint64` | Кумулятивная |

### Примеры запросов {#partitions-examples}

Топ-5 самых загруженных партиций среди всех таблиц базы данных:

```yql
SELECT
    Path,
    PartIdx,
    CPUCores
FROM `.sys/partition_stats`
ORDER BY CPUCores DESC
LIMIT 5
```

Список таблиц базы с размерами и нагрузкой в моменте:

```yql
SELECT
    Path,
    COUNT(*) as Partitions,
    SUM(RowCount) as Rows,
    SUM(DataSize) as Size,
    SUM(CPUCores) as CPU
FROM `.sys/partition_stats`
GROUP BY Path
```

Список таблиц базы с наибольшим числом сломанных блокировок:

```yql
SELECT
    Path,
    COUNT(*) as Partitions,
    SUM(LocksBroken) as TotalLocksBroken
FROM `.sys/partition_stats`
GROUP BY Path
ORDER BY TotalLocksBroken DESC
```

## Топы запросов {#top-queries}

Следующие системные представления хранят данные для анализа пользовательских запросов.

Наибольшее полное время выполнения запроса:

* `top_queries_by_duration_one_minute` — данные разбиты на минутные интервалы, содержит данные за последние 6 часов;
* `top_queries_by_duration_one_hour` — данные разбиты на часовые интервалы, содержит данные за последние 2 недели.

Наибольшее количество прочитанных из таблицы байт:

* `top_queries_by_read_bytes_one_minute` — данные разбиты на минутные интервалы, содержит данные за последние 6 часов;
* `top_queries_by_read_bytes_one_hour` — данные разбиты на часовые интервалы, содержит данные за последние 2 недели.

Наибольшее затраченное процессорное время:

* `top_queries_by_cpu_time_one_minute` — данные разбиты на минутные интервалы, содержит данные за последние 6 часов;
* `top_queries_by_cpu_time_one_hour` — данные разбиты на часовые интервалы, содержит данные за последние 2 недели.

Запросы с одним и тем же текстом объединяются, в выдачу попадает запрос с максимальным значением соответствующей метрики.
Каждый временной интервал (минута или час) содержит ТОП-5 запросов, выполненных в этот временной интервал.

Поля, предоставляющие информацию о затраченном процессорном времени (...`CPUTime`), выражены в микросекундах.

Текст запроса ограничен 10 килобайтами.

Все представления имеют одинаковую структуру:

| Колонка | Описание |
| --- | --- |
| `IntervalEnd` | Момент окончания минутного или часового интервала, за который собрана статистика.<br/>Тип: `Timestamp`.<br/>Ключ: `0`. |
| `Rank` | Ранг запроса в топе.<br/>Тип: `Uint32`.<br/>Ключ: `1`. |
| `QueryText` | Текст запроса.<br/>Тип: `Utf8`. |
| `Duration` | Полное время исполнения запроса.<br/>Тип: `Interval`. |
| `EndTime` | Момент окончания исполнения запроса. <br/>Тип: `Timestamp`. |
| `Type` | Тип запроса ("data", "scan", "script").<br/>Тип: `String`. |
| `ReadRows` | Количество прочитанных строк.<br/>Тип: `Uint64`. |
| `ReadBytes` | Количество прочитанных байт.<br/>Тип: `Uint64`. |
| `UpdateRows` | Количество записанных строк.<br/>Тип: `Uint64`. |
| `UpdateBytes` | Количество записанных байт.<br/>Тип: `Uint64`. |
| `DeleteRows` | Количество удалённых строк.<br/>Тип: `Uint64`. |
| `DeleteBytes` | Количество удалённых байт.<br/>Тип: `Uint64`. |
| `Partitions` | Количество партиций таблиц, участвовавших в исполнении запроса.<br/>Тип: `Uint64`. |
| `UserSID` | Security ID пользователя.<br/>Тип: `String`. |
| `ParametersSize` | Размер параметров запроса в байтах.<br/>Тип: `Uint64`. |
| `CompileDuration` | Длительность компиляции запроса.<br/>Тип: `Interval`. |
| `FromQueryCache` | Использовался ли кэш подготовленных запросов.<br/>Тип: `Bool`. |
| `CPUTime` | Общее процессорное время, использованное для исполнения запроса (микросекунды).<br/>Тип: `Uint64`. |
| `ShardCount` | Количество шардов, участвующих в исполнении запроса.<br/>Тип: `Uint64`. |
| `SumShardCPUTime` | Общее процессорное время, затраченное в шардах.<br/>Тип: `Uint64`. |
| `MinShardCPUTime` | Минимальное процесорное время, затраченное в шардах.<br/>Тип: `Uint64`. |
| `MaxShardCPUTime` | Максимальное процессорное время, затраченное в шардах.<br/>Тип: `Uint64`. |
| `ComputeNodesCount` | Количество вычислительных нод, задействованных в исполнении запроса.<br/>Тип: `Uint64`. |
| `SumComputeCPUTime` | Общее процессорное время, затраченное в вычислительных нодах.<br/>Тип: `Uint64`. |
| `MinComputeCPUTime` | Минимальное процессорное время, затраченное в вычислительных нодах.<br/>Тип: `Uint64`. |
| `MaxComputeCPUTime` | Максимальное процессорное время, затраченное в вычислительных нодах.<br/>Тип: `Uint64`. |
| `CompileCPUTime` | Процессорное время, затраченное на компиляцию запроса.<br/>Тип: `Uint64`. |
| `ProcessCPUTime` | Процессорное время, затраченное на общую обработку запроса.<br/>Тип: `Uint64`. |

### Примеры запросов {#top-queries-examples}

Топ запросов по времени выполнения. Запрос выполняется к представлению `.sys/top_queries_by_duration_one_minute`:

```yql
PRAGMA AnsiInForEmptyOrNullableItemsCollections;
$last = (
    SELECT
        MAX(IntervalEnd)
    FROM `.sys/top_queries_by_duration_one_minute`
);
SELECT
    IntervalEnd,
    Rank,
    QueryText,
    Duration
FROM `.sys/top_queries_by_duration_one_minute`
WHERE IntervalEnd IN $last
```

Запросы, прочитавшие больше всего байт. Запрос выполняется к представлению `.sys/top_queries_by_read_bytes_one_minute`:

```yql
SELECT
    IntervalEnd,
    QueryText,
    ReadBytes,
    ReadRows,
    Partitions
FROM `.sys/top_queries_by_read_bytes_one_minute`
WHERE Rank = 1
```

## Подробная информация о запросах {#query-metrics}

Следующее системное представление содержит подробную информацию о запросах:

* `query_metrics_one_minute` — данные разбиты по минутным интервалам, содержит до 256 запросов за последние 6 часов.

Каждая строка представления содержит информацию о множестве случившихся за интервал запросов с одинаковым текстом. Поля представления содержат минимальное, максимальное и суммарное значение по каждой отслеживаемой характеристике запроса. В пределах интервала запросы отсортированы по убыванию суммарного потраченного процессорного времени.

Ограничения:

* текст запроса ограничен 10 килобайтами;
* статистика может быть неполной, если база испытывает сильную нагрузку.

Структура представления:

| Колонка | Описание |
| ---|--- |
| `IntervalEnd` | Момент окончания минутного интервала, за который собрана статистика<br/>Тип: `Timestamp`.<br/>Ключ: `0`. |
| `Rank` | Ранг запроса в пределах интервала (по полю SumCPUTime).<br/>Тип: `Uint32`.<br/>Ключ: `1`. |
| `QueryText` | Текст запроса.<br/>Тип: `Utf8`. |
| `Count` | Количество запусков запроса.<br/>Тип: `Uint64`. |
| `SumDuration` | Общая длительность запросов.<br/>Тип: `Interval`. |
| `MinDuration` | Минимальная длительность запроса.<br/>Тип: `Interval`. |
| `MaxDuration` | Максимальная длительность запроса.<br/>Тип: `Interval`. |
| `SumCPUTime` | Общее затраченное процессорное время.<br/>Тип: `Uint64`. |
| `MinCPUTime` | Минимальное затраченное процессорное время.<br/>Тип: `Uint64`. |
| `MaxCPUTime` | Максимальное затраченное процессорное время.<br/>Тип: `Uint64`. |
| `SumReadRows` | Общее количество прочитанных строк.<br/>Тип: `Uint64`. |
| `MinReadRows` | Минимальное количество прочитанных строк.<br/>Тип: `Uint64`. |
| `MaxReadRows` | Максимальное количество прочитанных строк.<br/>Тип: `Uint64`. |
| `SumReadBytes` | Общее количество прочитанных байт.<br/>Тип: `Uint64`. |
| `MinReadBytes` | Минимальное количество прочитанных байт.<br/>Тип: `Uint64`. |
| `MaxReadBytes` | Максимальное количество прочитанных байт.<br/>Тип: `Uint64`. |
| `SumUpdateRows` | Общее количество записанных строк.<br/>Тип: `Uint64`. |
| `MinUpdateRows` | Минимальное количество записанных строк.<br/>Тип: `Uint64`. |
| `MaxUpdateRows` | Максимальное количество записанных строк.<br/>Тип: `Uint64`. |
| `SumUpdateBytes` | Общее количество записанных байт.<br/>Тип: `Uint64`. |
| `MinUpdateBytes` | Минимальное количество записанных байт.<br/>Тип: `Uint64`. |
| `MaxUpdateBytes` | Максимальное количество записанных байт.<br/>Тип: `Uint64`. |
| `SumDeleteRows` | Общее количество удалённых строк.<br/>Тип: `Uint64`. |
| `MinDeleteRows` | Минимальное количество удалённых строк.<br/>Тип: `Uint64`. |
| `MaxDeleteRows` | Максимальное количество удалённых строк.<br/>Тип: `Uint64`. |

### Примеры запросов {#query-metrics-examples}

Топ-10 запросов за последние 6 часов по общему количеству записанных строк в минутном интервале:

```yql
SELECT
    SumUpdateRows,
    Count,
    QueryText,
    IntervalEnd
FROM `.sys/query_metrics_one_minute`
ORDER BY SumUpdateRows DESC LIMIT 10
```

Недавние запросы, прочитавшие больше всего байт за минуту:

```yql
SELECT
    IntervalEnd,
    SumReadBytes,
    MinReadBytes,
    SumReadBytes / Count as AvgReadBytes,
    MaxReadBytes,
    QueryText
FROM `.sys/query_metrics_one_minute`
WHERE SumReadBytes > 0
ORDER BY IntervalEnd DESC, SumReadBytes DESC
LIMIT 100
```

## История перегруженных партиций {#top-overload-partitions}

Следующие системные представления содержат историю моментов высокой нагрузки на отдельные партиции таблиц БД:

* `top_partitions_one_minute` — данные разбиты на минутные интервалы, содержит историю за последние 6 часов;
* `top_partitions_one_hour` — данные разбиты на часовые интервалы, содержит историю за последние 2 недели.

В представления попадают партиции с пиковой нагрузкой более 70 % (`CPUCores` > 0,7). В пределах одного интервала партиции ранжированы по пиковому значению нагрузки.

Оба представления содержат одинаковый набор полей:

Ключами представления являются:

* `IntervalEnd` - момент окончания интервала;
* `Rank` - ранг партиции по пиковой нагрузке `CPUCores` в этом интервале.

Например, если в таблице есть 10 партиций, то `top_partitions_one_hour` для часового интервала `"20.12.2024 10:00-11:00"` выдаст 10 строк, отсортированных по порядку убывания `CPUCores`. У них будет `Rank` от 1 до 10 и одинаковый `IntervalEnd` `"20.12.2024 11:00"`.

| Колонка | Описание |
| --- | --- |
| `IntervalEnd` | Момент окончания минутного или часового интервала, за который собрана статистика.<br/>Тип: `Timestamp`.<br/>Ключ: `0`. |
| `Rank` | Ранг партиции в пределах интервала (по `CPUCores`).<br/>Тип: `Uint32`.<br/>Ключ: `1`. |
| `TabletId` | Идентификатор таблетки, обслуживающей партицию.<br/>Тип: `Uint64`. |
| `FollowerId` | Идентификатор [подписчика](../concepts/glossary.md#tablet-follower) таблетки партиции. Значение 0 означает лидера.<br/>Тип: `Uint32` |
| `Path` | Полный путь к таблице.<br/>Тип: `Utf8`. |
| `PeakTime` | Момент пикового значения в пределах интервала.<br/>Тип: `Timestamp`. |
| `CPUCores` | Пиковое значение нагрузки на партицию (доля времени ядра процессора, затраченного актором партиции).<br/>Тип: `Double`. |
| `NodeId` | Идентификатор ноды, на которой находилась партиция в момент пика.<br/>Тип: `Uint32`. |
| `DataSize` | Приблизительный размер партиции в байтах в момент пика.<br/>Тип: `Uint64`. |
| `RowCount` | Приблизительное количество строк в момент пика.<br/>Тип: `Uint64`. |
| `IndexSize` | Размер индекса партиции в таблетке в момент пика.<br/>Тип: `Uint64`. |
| `InFlightTxCount` | Количество транзакций, находящихся в процессе исполнения в момент пика.<br/>Тип: `Uint32`. |

### Примеры запросов {#top-overload-partitions-examples}

Следующий запрос выводит партиции с потреблением CPU более 70% в указанном интервале времени, с идентификаторами таблеток и их размерами на момент превышения. Запрос выполняется к представлению `.sys/top_partitions_one_minute`, которое содержит данные за последние 6 часов с разбиением по минутным интервалам:

```yql
SELECT
    IntervalEnd,
    CPUCores,
    Path,
    TabletId,
    DataSize
FROM `.sys/top_partitions_one_minute`
WHERE CPUCores > 0.7
AND IntervalEnd BETWEEN Timestamp("2000-01-01T00:00:00Z") AND Timestamp("2099-12-31T00:00:00Z")
ORDER BY IntervalEnd desc, CPUCores desc
```

Следующий запрос выводит партиции с потреблением CPU более 90% в указанном интервале времени, с идентификаторами таблеток и их размерами на момент превышения. Запрос выполняется к представлению `.sys/top_partitions_one_hour`, которое содержит данные за последние 2 недели с разбиением по часовым интервалам:

```yql
SELECT
    IntervalEnd,
    CPUCores,
    Path,
    TabletId,
    DataSize
FROM `.sys/top_partitions_one_hour`
WHERE CPUCores > 0.9
AND IntervalEnd BETWEEN Timestamp("2000-01-01T00:00:00Z") AND Timestamp("2099-12-31T00:00:00Z")
ORDER BY IntervalEnd desc, CPUCores desc
```

## История партиций со сломанными блокировками {#top-tli-partitions}

Следующие системные представления содержат историю моментов с ненулевым числом сломанных [блокировок](../contributor/datashard-locks-and-change-visibility.md) `LocksBroken` в отдельных партициях таблиц БД:

* `top_partitions_by_tli_one_minute` — данные разбиты на минутные интервалы, содержит историю за последние 6 часов;
* `top_partitions_by_tli_one_hour` — данные разбиты на часовые интервалы, содержит историю за последние 2 недели.

Представления выдают топ-10 партиций с ненулевым числом сломанных блокировок `LocksBroken`. В пределах одного интервала партиции ранжированы по числу сломанных блокировок `LocksBroken`.

Ключами представлений являются:

* `IntervalEnd` - момент окончания интервала;
* `Rank` - ранг партиции по числу сломанных блокировок `LocksBroken` в этом интервале.

Например, `top_partitions_by_tli_one_hour` для часового интервала `"20.12.2024 10:00-11:00"` выдаст 10 строк, отсортированных по порядку убывания `LocksBroken`. У них будет `Rank` от 1 до 10 и одинаковый `IntervalEnd` `"20.12.2024 11:00"`.

Оба представления содержат одинаковый набор полей:

| Колонка | Описание |
| --- | --- |
| `IntervalEnd` | Момент окончания минутного или часового интервала, за который собрана статистика.<br/>Тип: `Timestamp`.<br/>Ключ: `0`. |
| `Rank` | Ранг партиции в пределах интервала (по `LocksBroken`).<br/>Тип: `Uint32`.<br/>Ключ: `1`. |
| `TabletId` | Идентификатор таблетки, обслуживающей партицию.<br/>Тип: `Uint64`. |
| `FollowerId` | Идентификатор [подписчика](../concepts/glossary.md#tablet-follower) таблетки партиции. Значение 0 означает лидера.<br/>Тип: `Uint32` |
| `Path` | Полный путь к таблице.<br/>Тип: `Utf8`. |
| `LocksAcquired` | Число установленных блокировок "на диапазон ключей" в данном интервале.<br/>Тип: `Uint64`. |
| `LocksWholeShard` | Число установленных блокировок "на всю партицию" в данном интервале.<br/>Тип: `Uint64`. |
| `LocksBroken` | Число сломанных блокировок в данном интервале.<br/>Тип: `Uint64`. |
| `NodeId` | Идентификатор ноды, на которой находилась партиция в момент пика.<br/>Тип: `Uint32`. |
| `DataSize` | Приблизительный размер партиции в байтах в момент пика.<br/>Тип: `Uint64`. |
| `RowCount` | Приблизительное количество строк в момент пика.<br/>Тип: `Uint64`. |
| `IndexSize` | Размер индекса партиции в таблетке в момент пика.<br/>Тип: `Uint64`. |

### Примеры запросов {#top-tli-partitions-examples}

Следующий запрос выводит партиции в указанном интервале времени, с идентификаторами таблеток и числом сломанных блокировок. Запрос выполняется к представлению `.sys/top_partitions_by_tli_one_minute`:

```yql
SELECT
    IntervalEnd,
    LocksBroken,
    Path,
    TabletId
FROM `.sys/top_partitions_by_tli_one_hour`
WHERE IntervalEnd BETWEEN Timestamp("2000-01-01T00:00:00Z") AND Timestamp("2099-12-31T00:00:00Z")
ORDER BY IntervalEnd desc, LocksBroken desc
```

{% if feature_resource_pool %}

## Информация о пулах ресурсов {#resource_pools}

Системное представление `resource_pools` содержит информацию о [настройках](../yql/reference/syntax/create-resource-pool.md#parameters) [пулов ресурсов](../concepts/glossary.md#resource-pool).

Структура системного представления:

| Колонка | Описание |
| ------- | -------- |
| `Name` | Имя пула ресурсов.<br/>Тип: `Utf8`.<br/>Ключ: `0`. |
| `ConcurrentQueryLimit` | Максимальное количество параллельно выполняющихся запросов в пуле ресурсов.<br/>Тип: `Int32`. |
| `QueueSize` | Максимальный размер очереди ожидания.<br/>Тип: `Int32`. |
| `DatabaseLoadCpuThreshold` | Порог загрузки CPU всей базы данных, в процентах, после которого запросы не отправляются на выполнение и остаются в очереди.<br/>Тип: `Double`. |
| `ResourceWeight` | [Веса](../dev/resource-consumption-management.md#resources_weight) для распределения ресурсов между пулами.<br/>Тип: `Double`. |
| `TotalCpuLimitPercentPerNode` | Процент доступного CPU, который могут использовать все запросы на узле в данном пуле ресурсов.<br/>Тип: `Double`. |
| `QueryCpuLimitPercentPerNode` | Процент доступного CPU на узле для одного запроса в пуле ресурсов.<br/>Тип: `Double`. |
| `QueryMemoryLimitPercentPerNode` | Процент доступной памяти на узле, который может использовать запрос в данном пуле ресурсов.<br/>Тип: `Double`. |

### Пример {#resource_pools-examples}

Следующий запрос выводит информацию о настройках пула ресурсов с именем `default`:

```yql
SELECT
    Name,
    ConcurrentQueryLimit,
    QueueSize,
    DatabaseLoadCpuThreshold,
    ResourceWeight,
    TotalCpuLimitPercentPerNode,
    QueryCpuLimitPercentPerNode,
    QueryMemoryLimitPercentPerNode
FROM `.sys/resource_pools`
WHERE Name = "default";
```

{% endif %}

{% if feature_resource_pool_classifier %}

## Информация о классификаторах пулов ресурсов {#resource_pools_classifiers}

Системное представление `resource_pools_classifiers` содержит информацию о [настройках](../yql/reference/syntax/create-resource-pool-classifier.md#parameters) [классификаторов пулов ресурсов](../concepts/glossary.md#resource-pool-classifier).

Структура системного представления:

| Колонка | Описание |
| ------- | -------- |
| `Name` | Имя классификатора пула ресурсов.<br/>Тип: `Utf8`.<br/>Ключ: `0`. |
| `Rank` | Приоритет выбора классификатора пулов ресурсов.<br/>Тип: `Int64`. |
| `MemberName` | Пользователь или группа пользователей, которые будут отправлены в указанный пул ресурсов.<br/>Тип: `Utf8`. |
| `ResourcePool` | Имя пула ресурсов, в который будут отправлены запросы.<br/>Тип: `Utf8`. |

### Пример {#resource_pools_classifiers-examples}

Следующий запрос выводит информацию о настройках классификатора пула ресурсов с именем `olap`:

```yql
SELECT
    Name,
    Rank,
    MemberName,
    ResourcePool
FROM `.sys/resource_pools_classifiers`
WHERE Name = "olap";
```

{% endif %}

## Пользователи, группы и права доступа {#auth}

Следующие системные представления содержат информацию о пользователях, группах доступа, членстве пользователей в группах, а также о предоставленных правах доступа группам или непосредственно пользователям.

### Информация о пользователях {#users}

Представление `auth_users` содержит список локальных [пользователей](../concepts/glossary.md#access-user) {{ ydb-short-name }}. В него не входят пользователи, аутентифицированные через внешние системы, такие как LDAP.

Полный доступ к этому представлению имеют администраторы. Обычные пользователи могут просматривать только свои собственные данные.

Структура таблицы:

| Колонка | Описание |
|---------|----------|
| `Sid` | [SID](../concepts/glossary.md#sid) пользователя.<br />Тип: `Utf8`.<br />Ключ: `0`. |
| `IsEnabled` | Указывает, разрешён ли вход данному пользователю; используется для явной блокировки администратором. Независим от `IsLockedOut`.<br />Тип: `Bool`. |
| `IsLockedOut` | Указывает, что данный пользователь автоматически заблокирован из-за превышения количества неудачных аутентификаций. Не зависит от `IsEnabled`.<br />Тип: `Bool`. |
| `CreatedAt` | Время создания пользователя.<br />Тип: `Timestamp`. |
| `LastSuccessfulAttemptAt` | Время последней успешной аутентификации.<br />Тип: `Timestamp`. |
| `LastFailedAttemptAt` | Время последней неудачной аутентификации.<br />Тип: `Timestamp`. |
| `FailedAttemptCount` | Количество неудачных аутентификаций.<br />Тип: `Uint32`. |
| `PasswordHash` | JSON-строка, содержащая хеш пароля, соль и алгоритм хеширования.<br />Тип: `Utf8`. |

### Информация о группах

Представление `auth_groups` содержит список [групп доступа](../concepts/glossary.md#access-group).

Доступ к этому представлению имеют только администраторы.

Структура таблицы:

| Колонка | Описание |
|---------|----------|
| `Sid` | [SID](../concepts/glossary.md#sid) группы.<br />Тип: `Utf8`.<br />Ключ: `0`. |

### Информация о членстве в группах

Представление `auth_group_members` содержит информацию о членстве в [группах доступа](../concepts/glossary.md#access-group).

Доступ к этому представлению имеют только администраторы.

Структура таблицы:

| Колонка | Описание |
|---------|----------|
| `GroupSid` | SID группы.<br />Тип: `Utf8`.<br />Ключ: `0`. |
| `MemberSid` | SID участника группы. Может быть указан как SID пользователя, так и SID группы.<br />Тип: `Utf8`.<br />Ключ: `1`. |

### Информация о правах доступа

Представления содержат список выданных [прав доступа](../concepts/glossary.md#access-right).

Включают два представления:

* `auth_permissions`: Явно выданные права доступа.
* `auth_effective_permissions`: Эффективные права доступа с учётом [наследования](../concepts/glossary.md#access-right-inheritance).

Пользователю в данном представлении отображаются только те [объекты доступа](../concepts/glossary.md#access-object), на которые у него есть право `ydb.granular.describe_schema`.

Структура таблицы:

| Колонка | Описание |
|---------|----------|
| `Path` | Путь к объекту доступа.<br />Тип: `Utf8`.<br />Ключ: `0`. |
| `Sid` | SID [субъекта доступа](../concepts/glossary.md#access-subject).<br />Тип: `Utf8`.<br />Ключ: `1`. |
| `Permission` | Название [права доступа](../yql/reference/syntax/grant.md#permissions-list) {{ ydb-short-name }}.<br />Тип: `Utf8`.<br />Ключ: `2`. |

#### Примеры запросов

Получение явно предоставленных прав на объект доступа - таблицу `my_table`:

```yql
SELECT *
FROM `.sys/auth_permissions`
WHERE Path = "my_table"
```

Получение эффективных прав на объект доступа - таблицу `my_table`:

```yql
SELECT *
FROM `.sys/auth_effective_permissions`
WHERE Path = "my_table"
```

Получение прав, предоставленных пользователю `user3`:

```yql
SELECT *
FROM `.sys/auth_permissions`
WHERE Sid = "user3"
```

### Информация о владельцах объектов доступа {#auth-owners}

Представление `auth_owners` отображает информацию о [владельцах](../concepts/glossary.md#access-owner) [объектов доступа](../concepts/glossary.md#access-object).

Пользователю в данном представлении отображаются только те [объекты доступа](../concepts/glossary.md#access-object), на которые ему предоставлено право `ydb.granular.describe_schema`.

Структура таблицы:

| Колонка | Описание |
|---------|----------|
| `Path` | Путь к объекту доступа.<br />Тип: `Utf8`.<br />Ключ: `0`. |
| `Sid` | SID владельца объекта доступа.<br />Тип: `Utf8`. |
