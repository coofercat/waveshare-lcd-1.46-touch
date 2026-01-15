# waveshare-lcd-1.46-touch

 Added external component spd2010_glue, which glue the official lcd_touch driver and lcd_touch_spd2010.

## Shopping list
My device works as a fridge magenet, so I put shopping list in the middle.
Find the text sensor from the device, and create below automation in HA to sync shopping list item through.
```yaml
alias: Update Shopping list for waveshare list.
description: ""
triggers:
  - trigger: time_pattern
    seconds: /20
conditions: []
actions:
  - action: todo.get_items
    metadata: {}
    data:
      status: needs_action
    target:
      entity_id: todo.shopping_list
    response_variable: shopping_list_items
  - action: text.set_value
    metadata: {}
    target:
      entity_id: text.waveshare_va_shopping_list
    data:
      value: >-
        {% set items = shopping_list_items['todo.shopping_list']['items'] |
        default([]) %}

        {% if items | length > 0 %}     
          {{ '\u2022 ' ~ (items | map(attribute='summary') | list | join('\n\u2022 ')) }}     
        {% else %}
          Empty      
        {% endif %}
mode: single
```
