# oracle-commands.md â€” the exact commands that generated these fixtures

git.exe is the oracle of this folder, and this file is how anyone can check it.
The commands below ran on git version git version 2.56.0.windows.4 on this machine, in this order.
Run them in a scratch folder of their own; they make nothing of the repository.

The dates and the identity are pinned, so re-running the commands names the same
commit; the oids in loose-manifest.json are of the objects the commands below wrote.

```
set GIT_AUTHOR_DATE and GIT_COMMITTER_DATE to 2024-01-01T00:00:00 +0000

# the loose-object sample: a tiny repository, two blobs, one subtree.
git -C REPO init

# a1.txt and nested/sub.txt are placed in the workspace of REPO with the exact
# bytes of SRC/a1.txt and SRC/sub.txt â€” the files whose text is carried below
# fixtures/diff â€” and added, so git itself makes the index of the sample:
git -C REPO add -- a1.txt nested/sub.txt
git -C REPO ls-files --stage

# the commit over that index; the message is the one line of SRC/msg.txt.
git -C REPO -c user.name=FixtureMaker -c user.email=fixture@example.com commit -F SRC/msg.txt

# what the sample carries, read back from git and recorded in this file:
git -C REPO show-ref refs/heads/master
git -C REPO ls-tree -r refs/heads/master
git -C REPO ls-tree refs/heads/master
```

Every file under .git/objects whose name is two hex digits over a 38-hex-digit name
was copied, with that same two-digit subdirectory, to fixtures/loose/. The raw bytes
behind the zlib wrapper of each were inflated, hashed, and confirmed to hash to the
oid the file stands under; that is what loose-manifest.json states, per object.

```
# the packed sample: the same repository after gc.
git -C REPO gc
git -C REPO pack-refs --all --no-prune
```

The .pack and the .idx of .git/objects/pack were copied to fixtures/packed/ byte for
byte; a .rev file the same folder carried was not, because the sample is the packfile
and its index. fixtures/packed-refs-sample.txt is .git/packed-refs as git wrote it,
followed by comment lines â€” a line starting with # is a line git itself ignores â€”
recording the .git/HEAD content and whether the loose ref file is still there.

```
# the diff pairs: one pure insertion, one mixed edit and append.
git diff --no-index -U3 SRC/a1.txt SRC/a2.txt
git diff --no-index -U3 SRC/b1.txt SRC/b2.txt
```

Their output is fixtures/diff/a1-a2.diff and fixtures/diff/b1-b2.diff, with the pairs
they were made of beside them. git diff --no-index exits 1 when it finds differences,
which is why the generator reads its output and not its exit code.

## what the commands said on the run that made these fixtures

core.autocrlf is not set on this machine, so git converts nothing; the text files
above were written with LF endings anyway, and after the run every text fixture below
was read back byte for byte and found to carry no carriage return. The .pack and the
.idx were copied as bytes and never touched by that step.

```
100644 e0396ffc6284bb3b460e027c49d05bbb31b273e1 0	a1.txt
100644 f63ac6b34ade6c27cb84d19886049042a8bc2ce4 0	nested/sub.txt
a867ef798b215db19eaf0c7b615c789792fe62b9 refs/heads/master
100644 blob e0396ffc6284bb3b460e027c49d05bbb31b273e1	a1.txt
100644 blob f63ac6b34ade6c27cb84d19886049042a8bc2ce4	nested/sub.txt
100644 blob e0396ffc6284bb3b460e027c49d05bbb31b273e1	a1.txt
040000 tree 77626866b9dc450241b7ee8de07a25a327480109	nested
tree d25250f5f7f874caffa476f3a69161b879d2cbc6
author FixtureMaker <fixture@example.com> 1704067200 +0000
committer FixtureMaker <fixture@example.com> 1704067200 +0000

fixture objects: one commit, one tree with a subtree, two blobs
```
