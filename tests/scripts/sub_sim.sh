#!/usr/bin/env bash
# Simple subscriber using netcat (note: with netcat this is basic; better to use a small python client for real tests)
# This script opens an interactive nc session for demo; prefer send_pub.py to publish messages programmatically.
nc localhost 5000
# In the nc session type:
# HELLO SUBSCRIBER cli-test
# SUB sensors/test/environment
# then it will block and print binary messages (4-byte len + JSON)
