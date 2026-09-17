# Beanstalkd for the comparison in docs/benchmarks.md. There is no official
# image; this is the distribution's package, nothing else.
FROM alpine:3.20
RUN apk add --no-cache beanstalkd && mkdir -p /data
VOLUME ["/data"]
EXPOSE 11300
ENTRYPOINT ["beanstalkd", "-l", "0.0.0.0", "-p", "11300", "-b", "/data"]
