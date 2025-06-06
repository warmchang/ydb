# Транзакции и запросы к {{ ydb-short-name }}

Этот раздел описывает особенности реализации YQL для {{ ydb-short-name }} транзакций.

## Язык запросов {#query-language}

Основным средством создания, модификации и управления данными в {{ ydb-short-name }} является декларативный язык запросов YQL. YQL — это диалект SQL, который может считаться стандартом для общения с базами данных. Кроме того, {{ ydb-short-name }} поддерживает набор специальных RPC, например, для работы с древовидной схемой или для управления кластером.

## Режимы транзакций {#modes}

По умолчанию транзакции в {{ ydb-short-name }} выполняются в режиме *Serializable*, который предоставляет самый строгий [уровень изоляции](https://en.wikipedia.org/wiki/Isolation_(database_systems)#Serializable) для пользовательских транзакций. В этом режиме гарантируется, что результат успешно выполненных параллельных транзакций эквивалентен определенному последовательному порядку их выполнения, при этом для успешных транзакций отсутствуют [аномалии чтений](https://en.wikipedia.org/wiki/Isolation_(database_systems)#Read_phenomena).

В случае, когда требования консистентности или свежести читаемых транзакцией данных могут быть ослаблены, пользователь имеет возможность использовать режимы выполнения с пониженными гарантиями:

* *Online Read-Only* — каждое из чтений в транзакции читает последние на момент своего выполнения данные. Консистентность полученных данных определяется настройкой *allow_inconsistent_reads*:
  * *false* (consistent reads) — каждое из чтений по отдельности возвращает консистентные данные, но консистентность данных между разными чтениями не гарантируется. Дважды выполненное чтение одного и того же диапазона таблицы может вернуть разные результаты.
  * *true* (inconsistent reads) — данные даже для отдельно взятого чтения могут содержать неконсистентные результаты.
* *Stale Read-Only* — чтения данных в транзакции возвращают результаты с возможным отставанием от актуальных (доли секунды). Данные в каждом отдельно взятом чтении консистентны, между разными чтениями консистентность данных не гарантируется.
* *Snapshot Read-Only* — все чтения транзакции производятся из снапшота базы данных, при этом все чтения данных консистентны. Взятие снапшота происходит в момент старта транзакции, т.е. транзакция видит все изменения, закоммиченные до момента своего начала.

Режим выполнения транзакции задается в настройках транзакции при ее создании. Примеры для {{ ydb-short-name }} SDK смотрите в статье [{#T}](../../recipes/ydb-sdk/tx-control.md).

## Язык YQL {#language-yql}

Реализованные конструкции YQL можно разделить на два класса: [data definition language (DDL)](https://en.wikipedia.org/wiki/Data_definition_language) и [data manipulation language (DML)](https://en.wikipedia.org/wiki/Data_manipulation_language).

Подробнее о поддерживаемых конструкциях YQL можно почитать в [документации YQL](../../yql/reference/index.md).

Ниже перечислены возможности и ограничения поддержки YQL в {{ ydb-short-name }}, которые могут быть неочевидны на первый взгляд и на которые стоит обратить внимание:

* Допускаются multistatement transactions, то есть транзакции, состоящие из последовательности выражений YQL. При выполнении транзакции допускается взаимодействие с клиентской программой, иначе говоря, взаимодействие клиента с базой может выглядеть следующим образом: `BEGIN; выполнить SELECT; проанализировать результаты SELECT на клиенте; ...; выполнить UPDATE; COMMIT`. Стоит отметить, что если тело транзакции полностью сформировано до обращения к базе данных, то транзакция может обрабатываться эффективнее.
* В {{ ydb-short-name }} не поддерживается возможность смешивать DDL и DML запросы в одной транзакции. Традиционное понятие [ACID](https://ru.wikipedia.org/wiki/ACID) транзакции применимо именно к DML запросам, то есть к запросам, которые меняют данные. DDL запросы должны быть идемпотентными, то есть повторяемы в случае ошибки. Если необходимо выполнить действие со схемой, то каждое из действий будет транзакционно, а набор действий — нет.
* Реализация YQL в {{ ydb-short-name }} использует механизм [Optimistic Concurrency Control](https://en.wikipedia.org/wiki/Optimistic_concurrency_control). На затронутые в ходе транзакции сущности ставятся оптимистичные блокировки, при завершении транзакции проверяется, что блокировки не были инвалидированы. Оптимистичность блокировок выливается в важное для пользователя свойство — в случае конфликта выигрывает транзакция, которая завершается первой. Конкурирующие транзакции завершатся с ошибкой `Transaction locks invalidated`.
* Все изменения, производимые в рамках транзакции, накапливаются в памяти сервера базы данных и применяются в момент завершения транзакции. Если взятые блокировки не были инвалидированы, то все накопленные изменения применяются атомарно, если хотя бы одна блокировка была инвалидирована, то ни одно из изменений не будет применено. Описанная схема накладывает некоторые ограничения: объем изменений, осуществляемых в рамках одной транзакции, должен умещаться в оперативную память.

Для наиболее эффективного выполнения транзакций следует формировать их таким образом, чтобы в первой части транзакции выполнялись только чтения, а во второй части транзакции только модификации. Структура запроса тогда выглядит следующим образом:

```yql
       SELECT ...;
       ....
       SELECT ...;
       UPDATE/REPLACE/DELETE ...;
       COMMIT;
```

Подробнее о поддержке YQL в {{ ydb-short-name }} можно прочитать в [документации YQL](../../yql/reference/index.md).

## Распределенные транзакции {#distributed-tx}

[Таблица](../datamodel/table.md) в {{ ydb-short-name }} может быть шардирована по диапазонам значений первичного ключа. Различные шарды таблицы могут обслуживаться разными серверами распределенной БД (в том числе расположенными в разных локациях), а также могут независимо друг от друга перемещаться между серверами для перебалансировки или поддержания работоспособности шарда при отказах серверов или сетевого оборудования.

[Топик](../topic.md) в {{ ydb-short-name }} может быть шардирован на несколько партиций. Различные партиции топика, как и шарды таблицы, могут обслуживаться разными серверами распределенной БД.

В {{ ydb-short-name }} поддерживаются распределенные транзакции. Распределенные транзакции — это транзакции, которые затрагивают более одного шарда одной или нескольких таблиц и топиков. Они требуют больше ресурсов и выполняются дольше. В то время как точечные чтения и записи могут выполняться за время до 10 мс в 99 перцентиле, распределенные транзакции, как правило, занимают от 20 до 500 мс.

## Транзакции с участием топиков и таблиц {#topic-table-transactions}

{{ ydb-short-name }} поддерживает транзакции с участием [строковых таблиц](../glossary.md#row-oriented-table) и/или топиков. Таким образом, можно транзакционно перекладывать данные из таблиц в топики и в обратном направлении, а также между топиками, чтобы данные не терялись и не дублировались даже в случае непредвиденных обстоятельств.

Подробнее о транзакционных операциях при работе с топиками см. в [{#T}](../topic.md#topic-transactions) и [{#T}](../../reference/ydb-sdk/topic.md).