#!/usr/bin/env bash
echo "#!/boot/bin/sh" > $2
echo "# Generated by gen_test_wrapper.sh" >> $2
echo "run run_in_test_env $1" >> $2