# Mermaid diagrams

A flowchart with shapes, labels and a loop:

```mermaid
graph TD
    A[Start] --> B{Is it OK?}
    B -->|yes| C(Process)
    B -->|no| D[Fix it]
    D --> B
    C --> E([Done])
```

Left-to-right with a dotted and a thick link:

```mermaid
flowchart LR
    A((In)) --> B[Step one]
    B -- needs --> C{Choice}
    C -.-> D[Maybe]
    C ==> E[Definitely]
```

A wider graph (scroll it horizontally — drag the scrollbar, pan with the
mouse, or Shift+wheel):

```mermaid
graph LR
    A[Collect] --> B[Validate] --> C[Transform] --> D[Enrich] --> E[Store] --> F[Index] --> G[Serve]
```

An unsupported type renders as a labelled card (not raw code):

```mermaid
sequenceDiagram
    Alice->>Bob: Hello Bob
    Bob-->>Alice: Hi Alice
```
