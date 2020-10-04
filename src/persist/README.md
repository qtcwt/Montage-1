This folder contains infrastructures for fast recoverable data structures. For
pseudocode of (the original) epoch system, see:

https://docs.google.com/document/d/1J_hAxgGEVqVhe89moDAQskpgZQAUYICUoWUUBOYqK1U/edit


## Environment variables and values
usage: add argument:
```
-d<env>=<val>
```
to command line.

* `PersistStrat`: specify persist strategies
    * `DirWB`: directly write back every update to persistent blocks, and only issue an `sfence` on epoch advance
    * `PerEpoch`: keep to-be-persisted records of _whole epochs_ on a per-cache-line basis and flush them together
        * `Persister` = {`PerThreadWait`, `Advancer`}
        * `Container` = {`CircBuffer`, `Vector`, `HashSet`}
    * `BufferedWB`: keep to-be-persisted records of an epoch in a fixed-sized buffer and dump a (older) portion of them when it's full
        * `Persister` = {`PerThreadWait`, `PerThreadBusy`, `Worker`}
        * `BufferSize`
        * `DumpSize`
    * `No`: No persistence operations. NOTE: epoch advancing and all epoch-related persistency will be shut down. Overrides other environments.
* `TransTracker`: specify the type of active (data structure and bookkeeping) transaction tracker that prevents epoch advances if there are active transactions
    * `AtomicCounter`: a global atomic int active transaction counter for each epoch. lock-prefixed instruction on each update.
    * `ActiveThread`: per-thread true-false indicator of active threads on each recent epoch
    * `CurrEpoch`: per-thread indicator of current epoch on the thread
* `EpochLength`: specify epoch length.
* `EpochLengthUnit`: specify epoch length unit: `Second` (default) `Millisecond` or `Microsecond`.

## List of design-space-related experiments

* independent: write-flush same cache line vs. different cache line
* Typical combinations of PersistStrat + Persister:
    * DirWB
    * PerEpoch+{Advancer, WorkerThread, SingleDedicated, PerWorkerDedicated}
    * BufferedWB+{WorkerThread, SingleDedicated, PerWorkerDedicated}
        * BufferedWB's buffer size sensitivity test, with dumping size: full, half or 1.
* EpochAdvancer: Dedicated vs. SingleWorker
* EpochLength sensitivity test