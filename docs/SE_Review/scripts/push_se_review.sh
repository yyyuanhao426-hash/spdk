#!/bin/bash
cd /home/yin/nof/spdk || exit 1
git add docs/SE_Review
git -c user.name=LeeJace993 -c user.email=141028276+LeeJace993@users.noreply.github.com commit -m "docs(SE_Review): scen C rerun PASS after 043d1a48c reconnect fix (15 rounds, data verify OK)"
git -c credential.username=LeeJace993 push origin urma_enable_v2
