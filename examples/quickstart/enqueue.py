"""The producer from the README quickstart: python enqueue.py"""

import baton

client = baton.Client()
for name in ("ada", "grace", "edsger"):
    # When enqueue returns, the job is on disk. The key makes this script safe to
    # run twice: the same key returns the same job instead of creating another.
    job_id = client.enqueue_task("emails", "send_welcome", [f"{name}@example.com"],
                                 key=f"welcome-{name}")
    print(f"enqueued job {job_id} for {name}")
print(client.stats("emails"))
