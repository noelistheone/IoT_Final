"""Use the locally compiled image tool instead of an incompatible Intel binary."""
import os
Import("env")
tool = os.environ.get("AIRCUE_MKFS_BINARY")
if tool:
    env.Replace(MKFSTOOL=tool)
