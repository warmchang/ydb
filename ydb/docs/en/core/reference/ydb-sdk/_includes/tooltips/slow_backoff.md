**Slow exponential backoff** is one of the backoff strategies used by {{ ydb-short-name }} SDK when retrying queries that return an error.<br/>
The initial interval for this strategy is several **seconds**. For each subsequent attempt, the interval increases exponentially.<br/>
For more information, see [{#T}](../../error_handling.md#handling-retryable-errors).
