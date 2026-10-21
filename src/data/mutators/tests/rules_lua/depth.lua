-- Both spellings of the depth marker, as two blocks.
return {
on("weapons", "tdf"):change { ["<AllDepth2>"] = scale(2) },
on("weapons", "tdf"):change { ["<alldepth2>"] = scale(2) },
}
