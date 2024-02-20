#!/bin/bash

clear

java -jar /usr/bin/copybara.jar migrate games/idtech_1/copy.bara.sky gitlab
java -jar /usr/bin/copybara.jar migrate games/idtech_1/copy.bara.sky github
