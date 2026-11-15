# Personal blog

This site uses GitHub Pages with the Hacker Jekyll theme. To publish a post, add a Markdown file under `_posts/` named `YYYY-MM-DD-short-title.md`:

```markdown
---
layout: post
title: "A first post"
tags:
  - notes
  - writing
---

Write the post in Markdown below the front matter.
```

The date in the filename determines the post date. The home page lists posts newest-first, and `/tags/` groups posts by tag. Commit and push the file to the repository's GitHub Pages source branch to publish it.

Use exactly the `linux` and `debugging` tags on each post.

Store downloadable post assets under `assets/` and link to them using their site paths (for example, `/assets/example.pdf`).

Set `mermaid: true` in a post's front matter to render fenced Mermaid diagrams with mouse-wheel zoom, drag-to-pan, and on-screen controls.

Edit the text in `index.html` to personalize the introduction.
