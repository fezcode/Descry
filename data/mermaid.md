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

A sequence diagram:

```mermaid
sequenceDiagram
    Alice->>Bob: Hello Bob
    Bob-->>Alice: Hi Alice
```

Participants with aliases, notes, autonumber, frames and self messages:

```mermaid
sequenceDiagram
    autonumber
    actor U as User
    participant W as Web app
    participant A as Auth service
    participant DB as Database
    U->>W: Open login page
    W-->>U: Login form
    Note right of U: Types credentials
    U->>+W: Submit
    W->>A: Verify(user, pass)
    A->>DB: SELECT user
    DB-->>A: row
    alt valid password
        A-->>W: token
        W->>W: Store session
        W-->>U: Redirect to dashboard
    else invalid
        A--xW: 401
        W-->>-U: Show error
    end
    loop every 5 minutes
        W-)A: Refresh token
    end
    Note over W,A: Tokens expire after 1h
```

An unsupported type renders as a labelled card (not raw code):

```mermaid
classDiagram
    Animal <|-- Duck
    Animal : +int age
```
