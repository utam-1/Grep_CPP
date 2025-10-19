#!/usr/bin/env bash
# =============================================
# Regex Profiler Test Script
# =============================================

EXE="./grep"   # Adjust if your binary name differs
mkdir -p test_data

# Generate test input files
cat > test_data/basic.txt <<EOF
a
aa
aaa
aab
abc
b
EOF

cat > test_data/words.txt <<EOF
The quick brown fox
Regex engines are fun
12345
EOF

# Run profiling tests
declare -a PATTERNS=("a*b" "ab|cd" "[0-9]+" "^(The|A).+")

for PATTERN in "${PATTERNS[@]}"; do
  echo "--------------------------------"
  echo "Pattern: $PATTERN"
  echo "--------------------------------"
  $EXE -E "$PATTERN" test_data/basic.txt --profile > /dev/null
  $EXE -E "$PATTERN" test_data/words.txt --profile > /dev/null
  echo ""
done
