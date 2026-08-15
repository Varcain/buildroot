#!/bin/sh
set -e

base_url=${1:-http://172.1.1.1:8085}
wget -qO /tmp/sd-block-httpd "$base_url/sd-block-httpd"
wget -qO /tmp/sd-fat-apply "$base_url/sd-fat-apply"
chmod 755 /tmp/sd-block-httpd /tmp/sd-fat-apply
sha256sum /tmp/sd-block-httpd /tmp/sd-fat-apply
