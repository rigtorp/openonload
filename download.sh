#!/bin/sh

rclone --http-url='https://www.openonload.org/download/' -P --include 'openonload*' copy :http: .
