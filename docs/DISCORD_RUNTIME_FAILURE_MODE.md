# Discord runtime loader failure mode

On desktop platforms the Discord Social SDK uses a shared runtime library. If PulseForge is linked against that library but the library is omitted from the distributed package, the operating system can reject the executable before application startup. PulseForge's normal fail-open Discord error handling cannot catch a failure that happens before `main` is reached.

Accordingly, a Discord-enabled build is only releasable when its shared runtime is present in the final package and passes dependency inspection.
