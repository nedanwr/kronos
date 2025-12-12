#!/usr/bin/env bash
set -euo pipefail

IMAGE="${IMAGE:-kronos-dev:latest}"
CONTAINER_NAME="kronos-vg-$(date +%s)"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEMP_VOLUME="kronos-temp-$(date +%s)"

# Build image if missing
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "Building $IMAGE ..."
  docker build -t "$IMAGE" -f "$PROJECT_DIR/Dockerfile.dev" "$PROJECT_DIR"
fi

# Create temporary volume (will be destroyed after container exits)
echo "Creating temporary volume..."
docker volume create "$TEMP_VOLUME" > /dev/null

# Cleanup function to remove temp volume
cleanup() {
  echo "Cleaning up temporary volume..."
  docker volume rm "$TEMP_VOLUME" > /dev/null 2>&1 || true
}
trap cleanup EXIT

# Copy source code into temporary volume
echo "Copying source code into temporary volume..."
docker run --rm \
  -v "$PROJECT_DIR":/source:ro \
  -v "$TEMP_VOLUME":/work \
  "$IMAGE" \
  bash -c "cp -r /source/. /work/ && chmod -R 777 /work"

# Prefer your fast local script if present; fallback to running valgrind checks
CMD_IN_CONTAINER=${CMD_IN_CONTAINER:-""}
if [[ -z "$CMD_IN_CONTAINER" ]]; then
  if [[ -f "$PROJECT_DIR/scripts/valgrind_local.sh" ]]; then
    # Full suite, all cores in container
    CMD_IN_CONTAINER='bash -lc "make -j\$(nproc) && scripts/valgrind_local.sh --all --jobs \$(nproc)"'
  else
    # Run valgrind checks on all tests (same logic as CI)
    CMD_IN_CONTAINER='bash -c "
      set -e
      make clean
      make -j\$(nproc)
      
      check_with_valgrind() {
        local file=\$1
        local name=\$2
        local expect_error=\${3:-false}
        local logfile=\"/tmp/valgrind_\${name}.log\"
        
        if [ \"\$expect_error\" = \"true\" ]; then
          valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 --log-file=\"\$logfile\" ./kronos \"\$file\" > /dev/null 2>&1 || true
        else
          valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 --log-file=\"\$logfile\" ./kronos \"\$file\" > /dev/null 2>&1
        fi
        
        if grep -q \"All heap blocks were freed -- no leaks are possible\" \"\$logfile\" || \
           (grep -q \"definitely lost: 0 bytes in 0 blocks\" \"\$logfile\" && \
            grep -q \"indirectly lost: 0 bytes in 0 blocks\" \"\$logfile\"); then
          echo \"  ✓ \$name: No memory leaks\"
          rm -f \"\$logfile\"
          return 0
        else
          echo \"  ✗ \$name: Memory issues detected\"
          grep -A 5 \"ERROR SUMMARY\\|LEAK SUMMARY\\|definitely lost\\|indirectly lost\" \"\$logfile\" | head -20
          rm -f \"\$logfile\"
          return 1
        fi
      }
      
      failed_count=0
      
      echo \"=== Checking Passing Tests ===\"
      for test_file in tests/integration/pass/*.kr; do
        if [ -f \"\$test_file\" ]; then
          test_name=\$(basename \"\$test_file\" .kr)
          if ! check_with_valgrind \"\$test_file\" \"\$test_name\" false; then
            failed_count=\$((failed_count + 1))
          fi
        fi
      done
      
      echo \"\"
      echo \"=== Checking Error Tests ===\"
      for test_file in tests/integration/fail/*.kr; do
        if [ -f \"\$test_file\" ]; then
          test_name=\$(basename \"\$test_file\" .kr)
          if ! check_with_valgrind \"\$test_file\" \"\$test_name\" true; then
            failed_count=\$((failed_count + 1))
          fi
        fi
      done
      
      echo \"\"
      echo \"=== Memory Check Summary ===\"
      if [ \$failed_count -eq 0 ]; then
        echo \"✓ All files passed memory leak check\"
        exit 0
      else
        echo \"✗ \$failed_count file(s) failed memory leak check\"
        exit 1
      fi
    "'
  fi
fi

echo "Running Valgrind inside container $IMAGE (using temporary volume)..."
docker run --rm \
  --name "$CONTAINER_NAME" \
  -v "$TEMP_VOLUME":/work \
  -w /work \
  "$IMAGE" \
  bash -lc "$CMD_IN_CONTAINER"
