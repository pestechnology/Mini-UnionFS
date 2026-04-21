#!/bin/bash

FUSE_BINARY="./mini_unionfs"
TEST_DIR="./unionfs_test_env"
LOWER_DIR="$TEST_DIR/lower"
UPPER_DIR="$TEST_DIR/upper"
MOUNT_DIR="$TEST_DIR/mnt"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

PASS=0
FAIL=0

# ─────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────

section() {
    echo ""
    echo -e "${BOLD}${CYAN}══════════════════════════════════════════${NC}"
    echo -e "${BOLD}${CYAN}  $1${NC}"
    echo -e "${BOLD}${CYAN}══════════════════════════════════════════${NC}"
}

step() {
    echo -e "  ${YELLOW}▶  $1${NC}"
}

info() {
    echo -e "  ${DIM}     $1${NC}"
}

pass() {
    echo -e "  ${GREEN}  ✔  PASSED${NC} — $1"
    ((PASS++))
}

fail() {
    echo -e "  ${RED}  ✘  FAILED${NC} — $1"
    ((FAIL++))
}

show_file_content() {
    local label=$1
    local file=$2
    if [ -f "$file" ]; then
        echo -e "  ${DIM}     [$label] → \"$(cat $file | tr '\n' '|')\"${NC}"
    else
        echo -e "  ${DIM}     [$label] → (file does not exist)${NC}"
    fiz
}

show_dir_tree() {
    local label=$1
    local dir=$2
    echo -e "  ${DIM}     [$label]${NC}"
    if command -v tree &>/dev/null; then
        tree "$dir" 2>/dev/null | sed 's/^/          /'
    else
        ls -la "$dir" 2>/dev/null | sed 's/^/          /'
    fi
}

# ─────────────────────────────────────────────
# Header
# ─────────────────────────────────────────────

echo ""
echo -e "${BOLD}╔══════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║      Mini-UnionFS  Test Suite            ║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════╝${NC}"
echo ""


# ─────────────────────────────────────────────
# SETUP
# ─────────────────────────────────────────────

section "SETUP — Building Test Environment"

step "Cleaning up any previous test run..."
rm -rf "$TEST_DIR"
info "Removed: $TEST_DIR"

step "Creating directory structure..."
mkdir -p "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR"
info "lower/  → read-only base layer  : $LOWER_DIR"
info "upper/  → read-write layer       : $UPPER_DIR"
info "mnt/    → unified mount point    : $MOUNT_DIR"

step "Populating lower layer with test files..."
echo "base_only_content" > "$LOWER_DIR/base.txt"
echo "to_be_deleted"     > "$LOWER_DIR/delete_me.txt"
info "Created: lower/base.txt       → \"base_only_content\""
info "Created: lower/delete_me.txt  → \"to_be_deleted\""

step "Verifying upper layer is empty (as expected)..."
upper_count=$(ls "$UPPER_DIR" | wc -l)
info "Files in upper/ before mount: $upper_count"

echo ""
step "State before mounting:"
show_dir_tree "lower/" "$LOWER_DIR"
show_dir_tree "upper/" "$UPPER_DIR"

step "Mounting Mini-UnionFS..."
$FUSE_BINARY "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2
info "FUSE process PID: $FUSE_PID"
info "Mount point: $MOUNT_DIR"

echo ""
step "State after mounting — what the user sees at mnt/:"
show_dir_tree "mnt/  (unified view)" "$MOUNT_DIR"


# ─────────────────────────────────────────────
# TEST 1: Layer Visibility
# ─────────────────────────────────────────────

section "TEST 1 — Layer Visibility"

echo -e "  ${DIM}  What we're testing:${NC}"
info "Files from lower/ should be visible through the mount point."
info "The unified view must show lower layer files even though"
info "they physically only exist in lower/, not in upper/ or mnt/."
echo ""

step "Reading base.txt from the mount point..."
mnt_content=$(cat "$MOUNT_DIR/base.txt" 2>/dev/null)
info "mnt/base.txt content → \"$mnt_content\""

step "Confirming file physically lives in lower/, NOT upper/..."
show_file_content "lower/base.txt" "$LOWER_DIR/base.txt"
if [ -f "$UPPER_DIR/base.txt" ]; then
    info "upper/base.txt → EXISTS (unexpected at this stage)"r
else
    info "upper/base.txt → does not exist (correct — CoW not triggered yet)"
fi

echo ""
if grep -q "base_only_content" "$MOUNT_DIR/base.txt" 2>/dev/null; then
    pass "base.txt from lower/ is visible through the mount"
else
    fail "base.txt from lower/ is NOT visible through the mount"
fi

# ─────────────────────────────────────────────
# TEST 2: Copy-on-Write
# ─────────────────────────────────────────────

section "TEST 2 — Copy-on-Write (CoW)"

echo -e "  ${DIM}  What we're testing:${NC}"
info "When we write to a lower-layer file through the mount,"
info "the system must:"
info "  1. Copy the file up to upper/ (CoW promotion)"
info "  2. Apply the write to upper/ only"
info "  3. Leave lower/ completely untouched"
echo ""

step "State of base.txt BEFORE write:"
show_file_content "mnt/base.txt  (seen by user)" "$MOUNT_DIR/base.txt"
show_file_content "upper/base.txt (upper layer)" "$UPPER_DIR/base.txt"
show_file_content "lower/base.txt (lower layer)" "$LOWER_DIR/base.txt"

echo ""
step "Writing 'modified_content' to mnt/base.txt..."
echo "modified_content" >> "$MOUNT_DIR/base.txt" 2>/dev/null
info "Command: echo 'modified_content' >> mnt/base.txt"

echo ""
step "State of base.txt AFTER write:"
show_file_content "mnt/base.txt  (seen by user)" "$MOUNT_DIR/base.txt"
show_file_content "upper/base.txt (upper layer)" "$UPPER_DIR/base.txt"
show_file_content "lower/base.txt (lower layer)" "$LOWER_DIR/base.txt"

echo ""
step "Checking results..."

mnt_has_mod=$(grep -c "modified_content" "$MOUNT_DIR/base.txt" 2>/dev/null)
upper_has_mod=$(grep -c "modified_content" "$UPPER_DIR/base.txt" 2>/dev/null)
lower_has_mod=$(grep -c "modified_content" "$LOWER_DIR/base.txt" 2>/dev/null)

info "mnt/base.txt   has 'modified_content': $mnt_has_mod  (want: 1)"
info "upper/base.txt has 'modified_content': $upper_has_mod (want: 1)"
info "lower/base.txt has 'modified_content': $lower_has_mod (want: 0)"

echo ""
if [ "$mnt_has_mod" -eq 1 ] && \
   [ "$upper_has_mod" -eq 1 ] && \
   [ "$lower_has_mod" -eq 0 ]; then
    pass "CoW worked — upper/ modified, lower/ left untouched"
else
    fail "CoW failed — check upper/ and lower/ states above"
fi


