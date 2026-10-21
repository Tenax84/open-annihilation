function mutate(ctx)
  return {
    units = function(units, ctx)
      units.RGBOT:remove()
    end
  }
end
