"""The worker from the README quickstart.

    pip install "git+https://github.com/GaganGutta/baton#subdirectory=sdk/python"
    python worker.py

Kill it with Ctrl-C while a job is running and start it again: the job is
delivered again once its lease has expired. Nothing that was enqueued is lost.
"""

import logging
import time

import baton

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
worker = baton.Worker(["emails"], concurrency=4, lease_ms=10_000)


@worker.task()
def send_welcome(address):
    job = baton.current_job()
    logging.info("job %d (attempt %d): sending a welcome email to %s", job.id, job.attempt, address)
    time.sleep(0.5)  # the SMTP server is slow today


if __name__ == "__main__":
    worker.run()  # until SIGTERM or Ctrl-C; running jobs are allowed to finish
