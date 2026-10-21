function mutate(ctx)
  return {
    units = function(units, ctx)
      ctx.features.RGBOT_DEAD.blocking = 0
    end
  }
end
