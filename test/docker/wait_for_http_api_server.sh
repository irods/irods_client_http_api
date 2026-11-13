#! /usr/bin/env bash

host=localhost
port=9000
max_attempts=15

for ((i = 1; i <= max_attempts; ++i)); do
    if nc -z -w 1 "$host" "$port"; then
        echo "Server is ready."
        exit 0
    fi

    sleep 1
done

echo "Server failed to start within the timeout." >&2
exit 1
