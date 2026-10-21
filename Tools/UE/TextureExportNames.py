"""Deterministic filenames for UE textures exported into one flat directory."""

import hashlib
from collections import Counter


def export_filenames(references):
    references = tuple(references)
    packages = {reference: reference.split(".", 1)[0] for reference in references}
    stems = {reference: package.rsplit("/", 1)[-1]
             for reference, package in packages.items()}
    counts = Counter(stem.casefold() for stem in stems.values())
    filenames = {}
    used = set()
    for reference in sorted(references):
        stem = stems[reference]
        if counts[stem.casefold()] > 1:
            digest = hashlib.sha256(packages[reference].casefold().encode("utf-8")).hexdigest()[:12]
            stem += "__" + digest
        filename = stem + ".png"
        if filename.casefold() in used:
            raise ValueError("UE texture export filename collision: " + filename)
        used.add(filename.casefold())
        filenames[reference] = filename
    return filenames
