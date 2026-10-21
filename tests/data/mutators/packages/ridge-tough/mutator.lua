function mutate(ctx)
  return {{
    units = function(units, ctx)
      units:where {{ commander = true }}:set {{ maxdamage = ctx.options.health }}
    end
  }}
end
